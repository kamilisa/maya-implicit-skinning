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
#ifndef IBL_CONTROLLER_TOOLS_HPP__
#define IBL_CONTROLLER_TOOLS_HPP__

// =============================================================================
namespace IBL {
// =============================================================================

float sigpos(float x, float slope1);

float signeg(float x, float slope0);

float dsig(float x, float slope);

}
// END IBL =====================================================================

#endif // IBL_CONTROLLER_TOOLS_HPP__
