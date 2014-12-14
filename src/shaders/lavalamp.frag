/*
 Implicit skinning
 Copyright (C) 2013 Rodolphe Vaillant, Loic Barthe, Florian Cannezin,
 Gael Guennebaud, Marie Paule Cani, Damien Rohmer, Brian Wyvill,
 Olivier Gourmel

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License 3 as published by
 the Free Software Foundation.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program. If not, see <http://www.gnu.org/licenses/>
 */
#define M_PI (3.141592)

uniform sampler2D tex_sampler;
varying vec3 normal;
varying vec3 eyevec;

vec2 compute_spherical_coordinates(vec3 d){
	vec3 u = vec3(d.x, 0.0, d.z);
	u = normalize(u);
	float cosphi = u.x;
	float sinphi = u.z;
	float costheta = dot(d,u);
	float sintheta = -d.y;
	float phi = atan(sinphi, cosphi)/(2.*M_PI) + 0.5;
	phi = min(max(0.,phi),1.);
	float theta = atan(sintheta,costheta)/M_PI + 0.5;
	theta = min(max(0.,theta),1.);
  return vec2(phi,theta);
}


void main(){
		 vec3 ref = reflect(eyevec,normal);
		 vec2 tcoord = compute_spherical_coordinates(ref);
//		 gl_FragColor = vec4(1.,normal.x*normal.x,0.,1.);
		 gl_FragColor = texture2D(tex_sampler,tcoord);
     gl_FragColor = gl_FragColor *  gl_FragColor;
     //gl_FragColor = vec4(tcoord.x,tcoord.y,0.,1.);
}
