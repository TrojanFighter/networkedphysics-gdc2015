#include "SyncDemo.h"

#ifdef CLIENT

#include "stdlib.h"
#include "Cubes.h"
#include "Global.h"
#include "Font.h"
#include "Snapshot.h"
#include "FontManager.h"
#include "protocol/Stream.h"
#include "protocol/PacketFactory.h"
#include "network/Simulator.h"
#include <algorithm>

static const int MaxCubesPerPacket = 63;
static const int NumStateUpdates = 256;
static const int RightPort = 1001;

enum SyncMode
{
    SYNC_MODE_INPUT_ONLY,
    SYNC_MODE_INPUT_DESYNC,
    SYNC_MODE_INPUT_AND_STATE,
    SYNC_MODE_QUANTIZE,
    SYNC_MODE_SMOOTHING,
    SYNC_NUM_MODES
};

const char * sync_mode_descriptions[]
{
    "Input Only",
    "Input Desync",
    "Input and State",
    "Quantize",
    "Smoothing"
};

struct SyncModeData
{
    float playout_delay = 0.035f;                   // handle +/- two frames jitter @ 60 fps
    float latency = 0.0f;
    float packet_loss = 5.0f;
    float jitter = 2 * 1/60.0f;
};

static SyncModeData sync_mode_data[SYNC_NUM_MODES];

static void InitSyncModes()
{
    sync_mode_data[SYNC_MODE_INPUT_ONLY].packet_loss = 0.0f;
    sync_mode_data[SYNC_MODE_INPUT_ONLY].jitter = 0.0f;
}

enum SyncPackets
{
    SYNC_STATE_PACKET,
    SYNC_NUM_PACKETS
};

struct StateUpdate
{
    game::Input input;
    uint16_t sequence = 0;
    int num_cubes = 0;
    int cube_index[MaxCubesPerPacket];
    QuantizedCubeState_HighPrecision cube_state[MaxCubesPerPacket];
};

template <typename Stream> void serialize_cube_state_update( Stream & stream, int & index, QuantizedCubeState_HighPrecision & cube )
{
    serialize_int( stream, index, 0, NumCubes - 1 );

    serialize_int( stream, cube.position_x, -QuantizedPositionBoundXY_HighPrecision, +QuantizedPositionBoundXY_HighPrecision - 1 );
    serialize_int( stream, cube.position_y, -QuantizedPositionBoundXY_HighPrecision, +QuantizedPositionBoundXY_HighPrecision - 1 );
    serialize_int( stream, cube.position_z, 0, QuantizedPositionBoundZ_HighPrecision - 1 );

    serialize_object( stream, cube.orientation );

    bool at_rest = Stream::IsWriting ? cube.AtRest() : false;
    
    serialize_bool( stream, at_rest );

    if ( !at_rest )
    {
        serialize_int( stream, cube.linear_velocity_x, -QuantizedLinearVelocityBound_HighPrecision, +QuantizedLinearVelocityBound_HighPrecision - 1 );
        serialize_int( stream, cube.linear_velocity_y, -QuantizedLinearVelocityBound_HighPrecision, +QuantizedLinearVelocityBound_HighPrecision - 1 );
        serialize_int( stream, cube.linear_velocity_z, -QuantizedLinearVelocityBound_HighPrecision, +QuantizedLinearVelocityBound_HighPrecision - 1 );

        serialize_int( stream, cube.angular_velocity_x, -QuantizedAngularVelocityBound_HighPrecision, +QuantizedAngularVelocityBound_HighPrecision - 1 );
        serialize_int( stream, cube.angular_velocity_y, -QuantizedAngularVelocityBound_HighPrecision, +QuantizedAngularVelocityBound_HighPrecision - 1 );
        serialize_int( stream, cube.angular_velocity_z, -QuantizedAngularVelocityBound_HighPrecision, +QuantizedAngularVelocityBound_HighPrecision - 1 );
    }
    else if ( Stream::IsReading )
    {
        cube.linear_velocity_x = 0;
        cube.linear_velocity_y = 0;
        cube.linear_velocity_z = 0;

        cube.angular_velocity_x = 0;
        cube.angular_velocity_y = 0;
        cube.angular_velocity_z = 0;
    }
}

struct StatePacket : public protocol::Packet
{
    StateUpdate state_update;

    StatePacket() : Packet( SYNC_STATE_PACKET ) {}

    PROTOCOL_SERIALIZE_OBJECT( stream )
    {
        serialize_bool( stream, state_update.input.left );
        serialize_bool( stream, state_update.input.right );
        serialize_bool( stream, state_update.input.up );
        serialize_bool( stream, state_update.input.down );
        serialize_bool( stream, state_update.input.push );
        serialize_bool( stream, state_update.input.pull );

        serialize_uint16( stream, state_update.sequence );

        serialize_int( stream, state_update.num_cubes, 0, MaxCubesPerPacket );

        for ( int i = 0; i < state_update.num_cubes; ++i )
        {
            serialize_cube_state_update( stream, state_update.cube_index[i], state_update.cube_state[i] );
        }
    }
};

class StatePacketFactory : public protocol::PacketFactory
{
    core::Allocator * m_allocator;

public:

    StatePacketFactory( core::Allocator & allocator )
        : PacketFactory( allocator, SYNC_NUM_PACKETS )
    {
        m_allocator = &allocator;
    }

protected:

    protocol::Packet * CreateInternal( int type )
    {
        switch ( type )
        {
            case SYNC_STATE_PACKET:   return CORE_NEW( *m_allocator, StatePacket );
            default:
                return nullptr;
        }
    }
};

struct CubePriorityInfo
{
    int index;
    float accum;
};

struct StateJitterBuffer
{
    StateJitterBuffer( core::Allocator & allocator, const SyncModeData & mode_data )
        : state_updates( allocator, NumStateUpdates )
    {
        stopped = true;
        start_time = 0.0;
        playout_delay = mode_data.playout_delay;
    }

    void AddStateUpdate( double time, uint16_t sequence, const StateUpdate & state_update )
    {
        if ( stopped )
        {
            start_time = time;
            stopped = false;
        }

        auto entry = state_updates.Insert( sequence );

        if ( entry )
            memcpy( entry, &state_update, sizeof( state_update ) );
    }

    bool GetStateUpdate( double time, StateUpdate & state_update )
    {
        // we have not received a packet yet. no state update

        if ( stopped )
            return false;

        // if time minus playout delay is negative, it's too early for state updates

        time -= ( start_time + playout_delay );

        if ( time < 0 )
            return false;

        // if we are interpolating but the interpolation start time is too old,
        // go back to the not interpolating state, so we can find a new start point.

        const double frames_since_start = time * 60;        // note: locked to 60fps update rate

        uint16_t sequence = (uint16_t) uint64_t( floor( frames_since_start ) );

        auto entry = state_updates.Find( sequence );

        if ( !entry )
            return false;

        memcpy( &state_update, entry, sizeof( state_update ) );

        state_updates.Remove( sequence );

        return true;
    }

    void Reset()
    {
        stopped = true;
        start_time = 0.0;
        state_updates.Reset();
    }

    bool IsRunning( double time ) const
    {
        if ( stopped )
            return false;

        time -= ( start_time + playout_delay );

        return time >= 0;
    }

private:

    bool stopped;
    double start_time;
    float playout_delay;
    protocol::SequenceBuffer<StateUpdate> state_updates;
};

struct SyncInternal
{
    SyncInternal( core::Allocator & allocator, const SyncModeData & mode_data ) 
        : packet_factory( allocator )
    {
        this->allocator = &allocator;
        network::SimulatorConfig networkSimulatorConfig;
        networkSimulatorConfig.packetFactory = &packet_factory;
        networkSimulatorConfig.maxPacketSize = 4096;
        network_simulator = CORE_NEW( allocator, network::Simulator, networkSimulatorConfig );
        jitter_buffer = CORE_NEW( allocator, StateJitterBuffer, allocator, mode_data );
        Reset( mode_data );
    }

    ~SyncInternal()
    {
        CORE_ASSERT( network_simulator );
        typedef network::Simulator NetworkSimulator;
        CORE_DELETE( *allocator, NetworkSimulator, network_simulator );
        CORE_DELETE( *allocator, StateJitterBuffer, jitter_buffer );
        network_simulator = nullptr;
        jitter_buffer = nullptr;
    }

    void Reset( const SyncModeData & mode_data )
    {
        network_simulator->Reset();
        network_simulator->ClearStates();
        network_simulator->AddState( { mode_data.latency, mode_data.jitter, mode_data.packet_loss } );
        jitter_buffer->Reset();
        send_sequence = 0;
        for ( int i = 0; i < NumCubes; ++i )
        {
            priority_info[i].index = i;
            priority_info[i].accum = 0.0f;
            position_error[i] = vectorial::vec3f(0,0,0);
            orientation_error[i] = vectorial::quat4f(0,0,0,1);
        }
    }

    core::Allocator * allocator;
    uint16_t send_sequence;
    game::Input remote_input;
    bool disable_packets = false;
    network::Simulator * network_simulator;
    StatePacketFactory packet_factory;
    CubePriorityInfo priority_info[NumCubes];
    StateJitterBuffer * jitter_buffer;
    vectorial::vec3f position_error[NumCubes];
    vectorial::quat4f orientation_error[NumCubes];
};

SyncDemo::SyncDemo( core::Allocator & allocator )
{
    InitSyncModes();

    m_allocator = &allocator;
    m_internal = nullptr;
    m_settings = CORE_NEW( *m_allocator, CubesSettings );
    m_sync = CORE_NEW( *m_allocator, SyncInternal, *m_allocator, sync_mode_data[GetMode()] );
}

SyncDemo::~SyncDemo()
{
    Shutdown();

    CORE_DELETE( *m_allocator, SyncInternal, m_sync );
    CORE_DELETE( *m_allocator, CubesSettings, m_settings );

    m_sync = nullptr;
    m_settings = nullptr;
    m_allocator = nullptr;
}

bool SyncDemo::Initialize()
{
    if ( m_internal )
        Shutdown();

    m_internal = CORE_NEW( *m_allocator, CubesInternal );    

    CubesConfig config;
    
    config.num_simulations = 2;
    config.num_views = 2;

    m_internal->Initialize( *m_allocator, config, m_settings );

    return true;
}

void SyncDemo::Shutdown()
{
    CORE_ASSERT( m_allocator );

    CORE_ASSERT( m_sync );
    m_sync->Reset( sync_mode_data[GetMode()] );

    if ( m_internal )
    {
        m_internal->Free( *m_allocator );
        CORE_DELETE( *m_allocator, CubesInternal, m_internal );
        m_internal = nullptr;
    }
}

void ClampSnapshot( QuantizedSnapshot_HighPrecision & snapshot )
{
    for ( int i = 0; i < NumCubes; ++i )
    {
        QuantizedCubeState_HighPrecision & cube = snapshot.cubes[i];

        cube.position_x = core::clamp( cube.position_x, -QuantizedPositionBoundXY_HighPrecision, +QuantizedPositionBoundXY_HighPrecision - 1 );
        cube.position_y = core::clamp( cube.position_y, -QuantizedPositionBoundXY_HighPrecision, +QuantizedPositionBoundXY_HighPrecision - 1 );
        cube.position_z = core::clamp( cube.position_z, 0, +QuantizedPositionBoundZ_HighPrecision - 1 );

        cube.linear_velocity_x = core::clamp( cube.linear_velocity_x, -QuantizedLinearVelocityBound_HighPrecision, +QuantizedLinearVelocityBound_HighPrecision - 1 );
        cube.linear_velocity_y = core::clamp( cube.linear_velocity_y, -QuantizedLinearVelocityBound_HighPrecision, +QuantizedLinearVelocityBound_HighPrecision - 1 );
        cube.linear_velocity_z = core::clamp( cube.linear_velocity_z, -QuantizedLinearVelocityBound_HighPrecision, +QuantizedLinearVelocityBound_HighPrecision - 1 );

        cube.angular_velocity_x = core::clamp( cube.angular_velocity_x, -QuantizedAngularVelocityBound_HighPrecision, +QuantizedAngularVelocityBound_HighPrecision - 1 );
        cube.angular_velocity_y = core::clamp( cube.angular_velocity_y, -QuantizedAngularVelocityBound_HighPrecision, +QuantizedAngularVelocityBound_HighPrecision - 1 );
        cube.angular_velocity_z = core::clamp( cube.angular_velocity_z, -QuantizedAngularVelocityBound_HighPrecision, +QuantizedAngularVelocityBound_HighPrecision - 1 );
    }
}

void ApplySnapshot( GameInstance & game_instance, QuantizedSnapshot_HighPrecision & snapshot )
{
    for ( int i = 0; i < NumCubes; ++i )
    {
        const int id = i + 1;

        hypercube::ActiveObject * active_object = game_instance.FindActiveObject( id );

        if ( active_object )
        {
            CubeState cube;
            snapshot.cubes[i].Save( cube );

            active_object->position = math::Vector( cube.position.x(), cube.position.y(), cube.position.z() );
            active_object->orientation = math::Quaternion( cube.orientation.w(), cube.orientation.x(), cube.orientation.y(), cube.orientation.z() );
            active_object->linearVelocity = math::Vector( cube.linear_velocity.x(), cube.linear_velocity.y(), cube.linear_velocity.z() );
            active_object->angularVelocity = math::Vector( cube.angular_velocity.x(), cube.angular_velocity.y(), cube.angular_velocity.z() );

            game_instance.MoveActiveObject( active_object );
        }
    }
}

void ApplyStateUpdate( GameInstance & game_instance, const StateUpdate & state_update )
{
    for ( int i = 0; i < state_update.num_cubes; ++i )
    {
        const int id = state_update.cube_index[i] + 1;

        hypercube::ActiveObject * active_object = game_instance.FindActiveObject( id );

        if ( active_object )
        {
            CubeState cube;

            state_update.cube_state[i].Save( cube );

            active_object->position = math::Vector( cube.position.x(), cube.position.y(), cube.position.z() );
            active_object->orientation = math::Quaternion( cube.orientation.w(), cube.orientation.x(), cube.orientation.y(), cube.orientation.z() );
            active_object->linearVelocity = math::Vector( cube.linear_velocity.x(), cube.linear_velocity.y(), cube.linear_velocity.z() );
            active_object->angularVelocity = math::Vector( cube.angular_velocity.x(), cube.angular_velocity.y(), cube.angular_velocity.z() );
            active_object->authority = cube.interacting ? 0 : MaxPlayers;
            active_object->enabled = !state_update.cube_state[i].AtRest();

            game_instance.MoveActiveObject( active_object );
        }
    }
}

void CalculateCubePriorities( float * priority, QuantizedSnapshot_HighPrecision & snapshot )
{
    const float BasePriority = 1.0f;
    const float PlayerPriority = 1000000.0f;
    const float InteractingPriority = 100.0f;

    for ( int i = 0; i < NumCubes; ++i )
    {
        priority[i] = BasePriority;

        if ( i == 0 )
            priority[i] += PlayerPriority;

        if ( snapshot.cubes[i].interacting )
            priority[i] += InteractingPriority;
    }
}

bool priority_sort_function( const CubePriorityInfo & a, const CubePriorityInfo & b ) { return a.accum > b.accum; }

struct SendCubeInfo
{
    int index;
    bool send;
};

void MeasureCubesToSend( QuantizedSnapshot_HighPrecision & snapshot, SendCubeInfo * send_cubes, int max_bytes )
{
    const int max_bits = max_bytes * 8;

    int bits = 0;

    for ( int i = 0; i < MaxCubesPerPacket; ++i )
    {
        protocol::MeasureStream stream( max_bytes * 2 );

        int id = send_cubes[i].index;

        serialize_cube_state_update( stream, id, snapshot.cubes[id] );

        const int bits_processed = stream.GetBitsProcessed();

        if ( bits + bits_processed < max_bits )
        {
            send_cubes[i].send = true;
            bits += bits_processed;
        }
    }
}

void SyncDemo::Update()
{
    // quantize and clamp simulation state if necessary

    QuantizedSnapshot_HighPrecision left_snapshot;

    GetQuantizedSnapshot_HighPrecision( m_internal->GetGameInstance( 0 ), left_snapshot );

    ClampSnapshot( left_snapshot );

    if ( GetMode() >= SYNC_MODE_QUANTIZE )
        ApplySnapshot( *m_internal->simulation[0].game_instance, left_snapshot );

    // quantize and clamp right simulation state

    QuantizedSnapshot_HighPrecision right_snapshot;

    GetQuantizedSnapshot_HighPrecision( m_internal->GetGameInstance( 1 ), right_snapshot );

    ClampSnapshot( right_snapshot );

    if ( GetMode() >= SYNC_MODE_QUANTIZE )
        ApplySnapshot( *m_internal->simulation[1].game_instance, right_snapshot );

    // calculate cube priorities and determine which cubes to send in packet

    float priority[NumCubes];

    CalculateCubePriorities( priority, left_snapshot );

    for ( int i = 0; i < NumCubes; ++i )
        m_sync->priority_info[i].accum += global.timeBase.deltaTime * priority[i];

    CubePriorityInfo priority_info[NumCubes];

    memcpy( priority_info, m_sync->priority_info, sizeof( CubePriorityInfo ) * NumCubes );

    std::sort( priority_info, priority_info + NumCubes, priority_sort_function );

    SendCubeInfo send_cubes[MaxCubesPerPacket];

    for ( int i = 0; i < MaxCubesPerPacket; ++i )
    {
        send_cubes[i].index = priority_info[i].index;
        send_cubes[i].send = false;
    }

    const int MaxCubeBytes = 500;

    MeasureCubesToSend( left_snapshot, send_cubes, MaxCubeBytes );

    int num_cubes_to_send = 0;

    for ( int i = 0; i < MaxCubesPerPacket; i++ )
    {
        if ( send_cubes[i].send )
        {
            m_sync->priority_info[send_cubes[i].index].accum = 0.0f;
            num_cubes_to_send++;
        }
    }

    // construct state packet containing cubes to be sent

    auto state_packet = (StatePacket*) m_sync->packet_factory.Create( SYNC_STATE_PACKET );

    auto local_input = m_internal->GetLocalInput();

    state_packet->state_update.input = local_input;
    state_packet->state_update.sequence = m_sync->send_sequence;

    if ( GetMode() >= SYNC_MODE_INPUT_AND_STATE )
    {
        state_packet->state_update.num_cubes = num_cubes_to_send;
        int j = 0;    
        for ( int i = 0; i < MaxCubesPerPacket; ++i )
        {
            if ( send_cubes[i].send )
            {
                state_packet->state_update.cube_index[j] = send_cubes[i].index;
                state_packet->state_update.cube_state[j] = left_snapshot.cubes[ send_cubes[i].index ];
                j++;
            }
        }
        CORE_ASSERT( j == num_cubes_to_send );
    }

    m_sync->network_simulator->SendPacket( network::Address( "::1", RightPort ), state_packet );

    m_sync->send_sequence++;

    // update the network simulator

    m_sync->network_simulator->Update( global.timeBase );

    // receive packets from the simulator (with latency, packet loss and jitter applied...)

    while ( true )
    {
        auto packet = m_sync->network_simulator->ReceivePacket();
        if ( !packet )
            break;

        if ( !m_sync->disable_packets )
        {
            const auto port = packet->GetAddress().GetPort();
            const auto type = packet->GetType();

            if ( type == SYNC_STATE_PACKET && port == RightPort )
            {
                auto state_packet = (StatePacket*) packet;

    //            printf( "add state update: %d\n", state_packet->sequence );

                m_sync->jitter_buffer->AddStateUpdate( global.timeBase.time, state_packet->state_update.sequence, state_packet->state_update );
            }
        }

        m_sync->packet_factory.Destroy( packet );
    }

    // push state update to right simulation if available

    StateUpdate state_update;

    if ( m_sync->jitter_buffer->GetStateUpdate( global.timeBase.time, state_update ) )
    {
        m_sync->remote_input = state_update.input;

        ApplyStateUpdate( *m_internal->simulation[1].game_instance, state_update );
    }

    // run the simulation

    CubesUpdateConfig update_config;

    update_config.sim[0].num_frames = 1;
    update_config.sim[0].frame_input[0] = local_input;

    update_config.sim[1].num_frames = m_sync->jitter_buffer->IsRunning( global.timeBase.time ) ? 1 : 0;
    update_config.sim[1].frame_input[0] = m_sync->remote_input;

    m_internal->Update( update_config );

    // reduce position and orientation error

    static const float PositionErrorTightness = 0.95f;
    static const float OrientationErrorTightness = 0.95f;

    const vectorial::quat4f identity = vectorial::quat4f::identity();

    for ( int i = 0; i < NumCubes; ++i )
    {
        if ( vectorial::length_squared( m_sync->position_error[i] ) >= 0.001f )
             m_sync->position_error[i] *= PositionErrorTightness;
        else
             m_sync->position_error[i] = vectorial::vec3f(0,0,0);

        if ( vectorial::dot( m_sync->orientation_error[i], identity ) < 0 )
             m_sync->orientation_error[i] = -m_sync->orientation_error[i];

        if ( fabs( vectorial::dot( m_sync->orientation_error[i], vectorial::quat4f::identity() ) ) > 0.001f )
            m_sync->orientation_error[i] = vectorial::slerp( 1.0f - OrientationErrorTightness, m_sync->orientation_error[i], identity );
        else
            m_sync->orientation_error[i] = identity;
    }
}

bool SyncDemo::Clear()
{
    return m_internal->Clear();
}

void SyncDemo::Render()
{
    // render cube simulations

    CubesRenderConfig render_config;

    render_config.render_mode = CUBES_RENDER_SPLITSCREEN;

    if ( GetMode() >= SYNC_MODE_SMOOTHING )
    {
        render_config.view[1].position_error = m_sync->position_error;
        render_config.view[1].orientation_error = m_sync->orientation_error;
    }

    m_internal->Render( render_config );

    // render bandwidth overlay

    const float bandwidth = m_sync->network_simulator->GetBandwidth();

    char bandwidth_string[256];
    if ( bandwidth < 1024 )
        snprintf( bandwidth_string, (int) sizeof( bandwidth_string ), "Bandwidth: %d kbps", (int) bandwidth );
    else
        snprintf( bandwidth_string, (int) sizeof( bandwidth_string ), "Bandwidth: %.2f mbps", bandwidth / 1000 );

    Font * font = global.fontManager->GetFont( "Bandwidth" );
    if ( font )
    {
        const float text_x = ( global.displayWidth - font->GetTextWidth( bandwidth_string ) ) / 2;
        const float text_y = 5;
        font->Begin();
        font->DrawText( text_x, text_y, bandwidth_string, Color( 0.27f,0.81f,1.0f ) );
        font->End();
    }
}

bool SyncDemo::KeyEvent( int key, int scancode, int action, int mods )
{
    if ( key == GLFW_KEY_X )
    {
        if ( action == GLFW_PRESS || action == GLFW_REPEAT )
        {
            m_sync->disable_packets = true;
        }
        else if ( action == GLFW_RELEASE )
        {
            m_sync->disable_packets = false;
        }
    }

    return m_internal->KeyEvent( key, scancode, action, mods );
}

bool SyncDemo::CharEvent( unsigned int code )
{
    // ...

    return false;
}

int SyncDemo::GetNumModes() const
{
    return SYNC_NUM_MODES;
}

const char * SyncDemo::GetModeDescription( int mode ) const
{
    return sync_mode_descriptions[mode];
}

#endif // #ifdef CLIENT
