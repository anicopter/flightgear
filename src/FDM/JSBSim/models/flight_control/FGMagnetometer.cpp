/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

 Module:       FGMagnetometer.cpp
 Author:       Matthew Chave
 Date started: August 2009

 ------------- Copyright (C) 2009 -------------

 This program is free software; you can redistribute it and/or modify it under
 the terms of the GNU Lesser General Public License as published by the Free Software
 Foundation; either version 2 of the License, or (at your option) any later
 version.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 details.

 You should have received a copy of the GNU Lesser General Public License along with
 this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 Place - Suite 330, Boston, MA  02111-1307, USA.

 Further information about the GNU Lesser General Public License can also be found on
 the world wide web at http://www.gnu.org.

FUNCTIONAL DESCRIPTION
--------------------------------------------------------------------------------

HISTORY
--------------------------------------------------------------------------------

%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
COMMENTS, REFERENCES,  and NOTES
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
INCLUDES
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/

#include "FGMagnetometer.h"
#include "simgear/magvar/coremag.hxx"
#include <ctime>

namespace JSBSim {

static const char *IdSrc = "$Id$";
static const char *IdHdr = ID_MAGNETOMETER;

/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
CLASS IMPLEMENTATION
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/


FGMagnetometer::FGMagnetometer(FGFCS* fcs, Element* element) : FGSensor(fcs, element),\
                                                               counter(0),
                                                               INERTIAL_UPDATE_RATE(1000)
{
  Propagate = fcs->GetExec()->GetPropagate();
  MassBalance = fcs->GetExec()->GetMassBalance();
  Inertial = fcs->GetExec()->GetInertial();
  
  Element* location_element = element->FindElement("location");
  if (location_element) vLocation = location_element->FindElementTripletConvertTo("IN");
  else {cerr << "No location given for magnetometer. " << endl; exit(-1);}

  vRadius = MassBalance->StructuralToBody(vLocation);

  Element* orient_element = element->FindElement("orientation");
  if (orient_element) vOrient = orient_element->FindElementTripletConvertTo("RAD");
  else {cerr << "No orientation given for magnetometer. " << endl;}

  Element* axis_element = element->FindElement("axis");
  if (axis_element) {
    string sAxis = element->FindElementValue("axis");
    if (sAxis == "X" || sAxis == "x") {
      axis = 1;
    } else if (sAxis == "Y" || sAxis == "y") {
      axis = 2;
    } else if (sAxis == "Z" || sAxis == "z") {
      axis = 3;
    } else {
      cerr << "  Incorrect/no axis specified for magnetometer; assuming X axis" << endl;
      axis = 1;
    }
  }

  CalculateTransformMatrix();

  //assuming date wont significantly change over a flight to affect mag field
  //would be better to get the date from the sim if its simulated...
  time_t rawtime;
  time( &rawtime );
  tm * ptm = gmtime ( &rawtime );

  int year = ptm->tm_year;
  if(year>100)
  {
    year-= 100;
  }
  //the months here are zero based TODO find out if the function expects 1s based
  date = (yymmdd_to_julian_days(ptm->tm_year,ptm->tm_mon,ptm->tm_mday));//Julian 1950-2049 yy,mm,dd
  updateInertialMag();

  Debug(0);
}
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

FGMagnetometer::~FGMagnetometer()
{
  Debug(1);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
void FGMagnetometer::updateInertialMag(void)
{
  counter++;
  if(counter > INERTIAL_UPDATE_RATE)//dont need to update every iteration
  {
      counter = 0;

      usedLat = (Propagate->GetGeodLatitudeRad());//radians, N and E lat and long are positive, S and W negative
      usedLon = (Propagate->GetLongitude());//radians
      usedAlt = (Propagate->GetGeodeticAltitude()*fttom*0.001);//km

      //this should be done whenever the position changes significantly (in nTesla)
      double magvar = calc_magvar( usedLat,
                                 usedLon,
                                 usedAlt,
                                 date,
                                 field );
  }
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

bool FGMagnetometer::Run(void )
{
  // There is no input assumed. This is a dedicated acceleration sensor.
  
  vRadius = MassBalance->StructuralToBody(vLocation);


  updateInertialMag();
  //Inertial magnetic field rotated to the body frame
  vMag = Propagate->GetTl2b() * FGColumnVector3(field[3], field[4], field[5]);

  //allow for sensor orientation
  vMag = mT * vMag;
  
  Input = vMag(axis);

  Output = Input; // perfect magnetometer

  // Degrade signal as specified

  if (fail_stuck) {
    Output = PreviousOutput;
    return true;
  }

  if (lag != 0.0)            Lag();       // models magnetometer lag
  if (noise_variance != 0.0) Noise();     // models noise
  if (drift_rate != 0.0)     Drift();     // models drift over time
  if (bias != 0.0)           Bias();      // models a finite bias
  if (gain != 0.0)           Gain();      // models a gain

  if (fail_low)  Output = -HUGE_VAL;
  if (fail_high) Output =  HUGE_VAL;

  if (bits != 0)             Quantize();  // models quantization degradation
//  if (delay != 0.0)          Delay();     // models system signal transport latencies

  Clip(); // Is it right to clip an magnetometer?
  return true;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGMagnetometer::CalculateTransformMatrix(void)
{
  double cp,sp,cr,sr,cy,sy;

  cp=cos(vOrient(ePitch)); sp=sin(vOrient(ePitch));
  cr=cos(vOrient(eRoll));  sr=sin(vOrient(eRoll));
  cy=cos(vOrient(eYaw));   sy=sin(vOrient(eYaw));


  mT(1,1) =  cp*cy;
  mT(1,2) =  cp*sy;
  mT(1,3) = -sp;

  mT(2,1) = sr*sp*cy - cr*sy;
  mT(2,2) = sr*sp*sy + cr*cy;
  mT(2,3) = sr*cp;

  mT(3,1) = cr*sp*cy + sr*sy;
  mT(3,2) = cr*sp*sy - sr*cy;
  mT(3,3) = cr*cp;

  
  // This transform is different than for FGForce, where we want a native nozzle
  // force in body frame. Here we calculate the body frame accel and want it in
  // the transformed magnetometer frame. So, the next line is commented out.
  // mT = mT.Inverse();
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
//    The bitmasked value choices are as follows:
//    unset: In this case (the default) JSBSim would only print
//       out the normally expected messages, essentially echoing
//       the config files as they are read. If the environment
//       variable is not set, debug_lvl is set to 1 internally
//    0: This requests JSBSim not to output any messages
//       whatsoever.
//    1: This value explicity requests the normal JSBSim
//       startup messages
//    2: This value asks for a message to be printed out when
//       a class is instantiated
//    4: When this value is set, a message is displayed when a
//       FGModel object executes its Run() method
//    8: When this value is set, various runtime state variables
//       are printed out periodically
//    16: When set various parameters are sanity checked and
//       a message is printed out when they go out of bounds

void FGMagnetometer::Debug(int from)
{
  string ax[4] = {"none", "X", "Y", "Z"};

  if (debug_lvl <= 0) return;

  if (debug_lvl & 1) { // Standard console startup message output
    if (from == 0) { // Constructor
      cout << "        Axis: " << ax[axis] << endl;
    }
  }
  if (debug_lvl & 2 ) { // Instantiation/Destruction notification
    if (from == 0) cout << "Instantiated: FGMagnetometer" << endl;
    if (from == 1) cout << "Destroyed:    FGMagnetometer" << endl;
  }
  if (debug_lvl & 4 ) { // Run() method entry print for FGModel-derived objects
  }
  if (debug_lvl & 8 ) { // Runtime state variables
  }
  if (debug_lvl & 16) { // Sanity checking
  }
  if (debug_lvl & 64) {
    if (from == 0) { // Constructor
      cout << IdSrc << endl;
      cout << IdHdr << endl;
    }
  }
}
}