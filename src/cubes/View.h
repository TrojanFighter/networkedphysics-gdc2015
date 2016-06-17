/*
    Networked Physics Demo

    Copyright © 2008 - 2016, The Network Protocol Company, Inc.

    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

        1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

        2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer 
           in the documentation and/or other materials provided with the distribution.

        3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived 
           from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef CUBES_VIEW_H
#define CUBES_VIEW_H

#include "ViewObject.h"
#include "vectorial/vec3f.h"
#include "vectorial/mat4f.h"
#include "vectorial/quat4f.h"
#include "core/Core.h"
#include <map>

namespace view
{
    struct Cube
    {
        float r,g,b,a;
        vectorial::mat4f transform;
        vectorial::mat4f inverse_transform;
    };
    
	struct Cubes
	{
	    Cubes()
	    {
	        numCubes = 0;
	    }

	    int numCubes;
	    Cube cube[MaxViewObjects];
	};

	struct ObjectUpdate
	{
		vectorial::vec3f position;
		vectorial::quat4f orientation;
		float scale;
 		uint32_t id : 20;
		uint32_t authority : core::BitsRequired<0,MaxPlayers+1>::result;
		uint32_t visible : 1;
	};

	struct Object
	{
		Object()
		{
			this->id = 0;
			authority = 0;
			remove = 0;
			visible = 0;
			blending = 0;
		}

		unsigned int id;
		int authority;
		float scale;
		float r,g,b,a;
		bool remove;
		bool visible;
		bool blending;
		float blend_time;
		float blend_start;
		float blend_finish;

		vectorial::vec3f position;
		vectorial::quat4f orientation;
		vectorial::vec3f linear_velocity;
		vectorial::vec3f angular_velocity;
	};

	/*	
		Manages the set of objects on the view-side which is typically
		a copy following the set of objects in the simulation active set
	*/
	
	class ObjectManager
	{
	public:

		void Reset();

		void UpdateObjects( ObjectUpdate updates[], int updateCount );
		
		void Update( float deltaTime );

		Object * GetObject( unsigned int id );

		void GetRenderState( Cubes & renderState, const vectorial::vec3f * position_error, const vectorial::quat4f * orientation_error );

	private:

		typedef std::map<unsigned int, Object*> object_map;
		object_map objects;
	};

	// camera object

	struct Camera
	{
		vectorial::vec3f position;
		vectorial::vec3f lookat;
		vectorial::vec3f up;

		Camera();
	
		void EaseIn( const vectorial::vec3f & new_lookat, const vectorial::vec3f & new_position );
	
		void Snap( const vectorial::vec3f & new_lookat, const vectorial::vec3f & new_position );
	};

	// helper functions

	void getAuthorityColor( int authority, float & r, float & g, float & b, int maxPlayers = MaxPlayers );

	void getViewObjectUpdates( view::ObjectUpdate * updates, const view::Packet & viewPacket );
}

#endif
