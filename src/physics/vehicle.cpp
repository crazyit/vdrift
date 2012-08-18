/************************************************************************/
/*                                                                      */
/* This file is part of VDrift.                                         */
/*                                                                      */
/* VDrift is free software: you can redistribute it and/or modify       */
/* it under the terms of the GNU General Public License as published by */
/* the Free Software Foundation, either version 3 of the License, or    */
/* (at your option) any later version.                                  */
/*                                                                      */
/* VDrift is distributed in the hope that it will be useful,            */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of       */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        */
/* GNU General Public License for more details.                         */
/*                                                                      */
/* You should have received a copy of the GNU General Public License    */
/* along with VDrift.  If not, see <http://www.gnu.org/licenses/>.      */
/*                                                                      */
/************************************************************************/

#include "vehicle.h"
#include "vehicleinfo.h"
#include "solveconstraintrow.h"
#include "world.h"
#include "coordinatesystem.h"
#include "BulletCollision/CollisionShapes/btCompoundShape.h"
#include "BulletCollision/CollisionShapes/btCylinderShape.h"

//static std::ofstream clog("log.txt");

namespace sim
{

// helper function to retrieve shaft from id
static Shaft * LinkShaft(
	int shaft_id,
	btAlignedObjectArray<Wheel> & wheel,
	btAlignedObjectArray<Differential> & diff)
{
	if (shaft_id < wheel.size())
	{
		btAssert(shaft_id >= 0);
		return &wheel[shaft_id].shaft;
	}
	shaft_id -= wheel.size();
	btAssert(shaft_id < diff.size());
	return &diff[shaft_id].getShaft();
}

Vehicle::Vehicle() :
	world(0),
	body(0),
	brake_value(0),
	last_clutch(1),
	remaining_shift_time(0),
	tacho_rpm(0),
	shift_gear(0),
	autoclutch(true),
	autoshift(false),
	shifted(true),
	abs_active(false),
	tcs_active(false),
	abs(false),
	tcs(false),
	maxangle(0),
	maxspeed(0)
{
	// Constructor
}

Vehicle::~Vehicle()
{
	if (!world) return;

	body->clear(*world);
	world->removeAction(this);
	world->removeRigidBody(body);
	delete body->getCollisionShape();
	delete body;
}

void Vehicle::init(
	const VehicleInfo & info,
	const btVector3 & position,
	const btQuaternion & rotation,
	World & world)
{
	transform.setRotation(rotation);
	transform.setOrigin(position);

	body = new FractureBody(info.body);
	body->setCenterOfMassTransform(transform);
	body->setActivationState(DISABLE_DEACTIVATION);
	body->setContactProcessingThreshold(0.0);
	body->setCollisionFlags(body->getCollisionFlags() | btCollisionObject::CF_CUSTOM_MATERIAL_CALLBACK);
	world.addRigidBody(body);
	world.addAction(this);
	this->world = &world;

	aero_device.resize(info.aerodevice.size());
	for (int i = 0; i < info.aerodevice.size(); ++i)
	{
		aero_device[i] = AeroDevice(info.aerodevice[i]);
	}

	antiroll = info.antiroll;
	differential.resize(info.differential.size());
	wheel.resize(info.wheel.size());
	wheel_contact.resize(wheel.size());
	diff_joint.resize(differential.size());
	clutch_joint.resize(differential.size() + 1);
	motor_joint.resize(wheel.size() * 2 + 1);

	for (int i = 0; i < wheel.size(); ++i)
	{
		wheel[i].init(info.wheel[i], world, *body);
		maxangle = btMax(maxangle, wheel[i].suspension.getMaxSteeringAngle());
	}

	for (int i = 0; i < differential.size(); ++i)
	{
		Shaft * shaft_a = LinkShaft(info.differential_link_a[i], wheel, differential);
		Shaft * shaft_b = LinkShaft(info.differential_link_b[i], wheel, differential);
		differential[i].init(info.differential[i], *shaft_a, *shaft_b);
	}

	Shaft * shaft_t = LinkShaft(info.transmission_link, wheel, differential);
	transmission.init(info.transmission, *shaft_t);
	clutch.init(info.clutch);
	engine.init(info.engine);

	calculateFrictionCoefficient(lon_friction_coeff, lat_friction_coeff);
	maxspeed = calculateMaxSpeed();

	// position is the center of a 2 x 4 x 1 meter box on track surface
	// move car to fit bounding box front lower edge of the position box
	btVector3 bmin, bmax;
	body->getCollisionShape()->getAabb(btTransform::getIdentity(), bmin, bmax);
	btVector3 fwd = body->getCenterOfMassTransform().getBasis().getColumn(1);
	btVector3 up = body->getCenterOfMassTransform().getBasis().getColumn(2);
	btVector3 fwd_offset = fwd * (2.0 - bmax.y());
	btVector3 up_offset = -up * (0.5 + bmin.z());
	setPosition(body->getCenterOfMassPosition() + up_offset + fwd_offset);
	//alignWithGround();
}

void Vehicle::debugDraw(btIDebugDraw*)
{
	// nothing to do here
}

void Vehicle::startEngine()
{
	//clutch.setPosition(0);
	engine.start();
}

void Vehicle::setGear(int value)
{
	if (shifted &&
		value != transmission.getGear() &&
		value <= transmission.getForwardGears() &&
		value >= -transmission.getReverseGears())
	{
		remaining_shift_time = transmission.getShiftTime();
		shift_gear = value;
		shifted = false;
	}
}

void Vehicle::setThrottle(btScalar value)
{
	engine.setThrottle(value);
}

void Vehicle::setNOS(btScalar value)
{
	engine.setNosBoost(value);
}

void Vehicle::setClutch(btScalar value)
{
	clutch.setPosition(value);
}

void Vehicle::setBrake(btScalar value)
{
	brake_value = value;
	for (int i = 0; i < wheel.size(); ++i)
	{
		wheel[i].brake.setBrakeFactor(value);
	}
}

void Vehicle::setHandBrake(btScalar value)
{
	for (int i = 0; i < wheel.size(); ++i)
	{
		wheel[i].brake.setHandbrakeFactor(value);
	}
}

void Vehicle::setAutoClutch(bool value)
{
	autoclutch = value;
}

void Vehicle::setAutoShift(bool value)
{
	autoshift = value;
}

const btTransform & Vehicle::getTransform() const
{
	return body->getCenterOfMassTransform();
}

const btVector3 & Vehicle::getPosition() const
{
	return body->getCenterOfMassPosition();
}

const btVector3 & Vehicle::getVelocity() const
{
	return body->getLinearVelocity();
}

const btScalar Vehicle::getInvMass() const
{
	return body->getInvMass();
}

const btCollisionObject * Vehicle::getCollisionObject() const
{
	return body;
}

const btCollisionWorld * Vehicle::getCollisionWorld() const
{
	return world;
}

btScalar Vehicle::getSpeed() const
{
	return body->getLinearVelocity().length();
}

btScalar Vehicle::getSpeedMPS() const
{
	return wheel[0].getRadius() * wheel[0].shaft.getAngularVelocity();
}

btScalar Vehicle::getMaxSpeedMPS() const
{
	return maxspeed;
}

btScalar Vehicle::getTachoRPM() const
{
	return tacho_rpm;
}

void Vehicle::setABS(bool value)
{
	abs = value;
	for (int i = 0; i < wheel.size(); ++i)
	{
		wheel[i].setABS(value);
	}
}

void Vehicle::setTCS(bool value)
{
	tcs = value;
	for (int i = 0; i < wheel.size(); ++i)
	{
		wheel[i].setTCS(value);
	}
}

void Vehicle::setPosition(const btVector3 & position)
{
	body->translate(position - body->getCenterOfMassPosition());
	transform.setOrigin(position);
}

void Vehicle::alignWithGround()
{
	btScalar ray_len = 8;
	btScalar min_height = 0;
	bool no_min_height = true;
	for (int i = 0; i < wheel.size(); ++i)
	{
		wheel[i].updateDisplacement(ray_len);
		btScalar height = wheel[i].ray.getDepth() - ray_len;
		if (height < min_height || no_min_height)
		{
			min_height = height;
			no_min_height = false;
		}
	}

	btVector3 delta = getDownVector() * min_height;
	btVector3 trimmed_position = body->getCenterOfMassPosition() + delta;
	setPosition(trimmed_position);
	for (int i = 0; i < wheel.size(); ++i)
	{
		wheel[i].updateDisplacement(ray_len);
	}

	body->setAngularVelocity(btVector3(0, 0, 0));
	body->setLinearVelocity(btVector3(0, 0, 0));
}

// ugh, ugly code
void Vehicle::rolloverRecover()
{
	btTransform transform = body->getCenterOfMassTransform();

	btVector3 z(direction::up);
	btVector3 y_car = transform.getBasis() * direction::forward;
	y_car = y_car - z * z.dot(y_car);
	y_car.normalize();

	btVector3 z_car = transform.getBasis() * direction::up;
	z_car = z_car - y_car * y_car.dot(z_car);
	z_car.normalize();

	btScalar angle = z_car.angle(z);
	if (fabs(angle) < M_PI / 4.0) return;

	btQuaternion rot(y_car, angle);
	rot = rot * transform.getRotation();
	transform.setRotation(rot);

	body->setCenterOfMassTransform(transform);

	alignWithGround();
}

void Vehicle::setSteering(const btScalar value)
{
	for (int i = 0; i < wheel.size(); ++i)
	{
		wheel[i].suspension.setSteering(value);
	}
}

btScalar Vehicle::getMaxSteeringAngle() const
{
	return maxangle;
}

btScalar Vehicle::getBrakingDistance(btScalar target_speed) const
{
	// Braking distance estimation (ignoring aerodynamic drag)
	// mu * mass * gravity * distance = 0.5 * mass * (Initial_velocity^2 - Final_velocity^2)
	// Distance is:
	// distance = (Initial_velocity^2 - Final_velocity^2) / (2 * mu * gravity)
	btScalar current_speed_2 = body->getLinearVelocity().length2();
	btScalar target_speed_2 = target_speed * target_speed;
	btScalar friction = lon_friction_coeff;
	btScalar gravity = 9.81;
	if (target_speed_2 < current_speed_2)
	{
		return (current_speed_2 - target_speed_2) / (2.0 * friction * gravity);
	}
	return 0;
}

btScalar Vehicle::getMaxVelocity(btScalar radius) const
{
	btScalar g = 9.81;
	btScalar im = body->getInvMass();
	btScalar d = 1.0 - btMin(btScalar(1.01), -radius * getLiftCoefficient() * lat_friction_coeff * im);
	btScalar real = lat_friction_coeff * g * radius / d;
	btScalar v = 1000.0;
	if (real > 0) v = sqrt(real);
	return v;
}

btVector3 Vehicle::getTotalAero() const
{
	btVector3 force(0, 0, 0);
	for (int i = 0; i < aero_device.size(); ++i)
	{
		force = force + aero_device[i].getLift() + aero_device[i].getDrag();
	}
	return force;
}

btScalar Vehicle::getLiftCoefficient() const
{
	btScalar coeff = 0.0;
	for (int i = 0; i < aero_device.size(); ++i)
	{
		coeff += aero_device[i].getLiftCoefficient();
	}
	return coeff;
}

btScalar Vehicle::getDragCoefficient() const
{
	btScalar coeff = 0.0;
	for (int i = 0; i < aero_device.size(); ++i)
	{
		coeff += aero_device[i].getDragCoefficient();
	}
	return coeff;
}

btScalar Vehicle::getFeedback() const
{
	return feedback;
}

btVector3 Vehicle::getDownVector() const
{
	return -body->getCenterOfMassTransform().getBasis().getColumn(2);
}

void Vehicle::updateAction(btCollisionWorld * collisionWorld, btScalar dt)
{
	updateAerodynamics(dt);

	updateTransmission(dt);

	engine.update(dt);

	updateDynamics(dt);

	tacho_rpm = engine.getRPM() * 0.3 + tacho_rpm * 0.7;

	body->setCenterOfMassTransform(transform);
	body->predictIntegratedTransform(dt, transform);
	body->proceedToTransform(transform);
	updateWheelTransform(dt);
}

void Vehicle::updateDynamics(btScalar dt)
{
	// differentials (constant, should happen at initialisation maybe?)
	for (int i = 0; i < differential.size(); ++i)
	{
		DifferentialJoint & djoint = diff_joint[i];
		djoint.shaft1 = &differential[i].getShaft();
		djoint.shaft2a = &differential[i].getShaft1();
		djoint.shaft2b = &differential[i].getShaft2();
		djoint.gearRatio = differential[i].getFinalDrive();
		djoint.init();

		ClutchJoint & cjoint = clutch_joint[i];
		cjoint.shaft1 = &differential[i].getShaft1();
		cjoint.shaft2 = &differential[i].getShaft2();
		cjoint.gearRatio = 1;
		cjoint.impulseLimit = differential[i].getAntiSlipTorque() * dt;
		cjoint.init();
	}

	// transmission and clutch
	int dcount = differential.size();
	int ccount = differential.size();
	//if (btFabs(transmission.getGearRatio()) > 0.0)
	{
		ClutchJoint & cjoint = clutch_joint[ccount];
		cjoint.shaft1 = &engine.getShaft();
		cjoint.shaft2 = &transmission.getShaft();
		cjoint.gearRatio = transmission.getGearRatio();
		cjoint.impulseLimit = transmission.getGearRatio() != 0 ? clutch.getTorque() * dt : 0;
		cjoint.init();
		ccount++;
	}

	// wheel displacement
	for (int i = 0; i < wheel.size(); ++i)
	{
		wheel[i].updateDisplacement(2 * wheel[i].getRadius());
	}

	// anti-roll bar approximation by adjusting suspension stiffness
	for (int i = 0; i < antiroll.size(); ++i)
	{
		// move this into antirollbar class ?

		// calculate anti-roll contributed stiffness
		btScalar kr = antiroll[i].stiffness;
		int i0 = antiroll[i].wheel0;
		int i1 = antiroll[i].wheel1;
		btScalar d0 = wheel[i0].suspension.getDisplacement();
		btScalar d1 = wheel[i1].suspension.getDisplacement();
		btScalar dr = d0 - d1;
		btScalar k0 = (d0 > 0) ? kr * dr / d0 : 0.0f;
		btScalar k1 = (d1 > 0) ? -kr * dr / d1 : 0.0f;

		// avoid negative stiffness
		if (wheel[i0].suspension.getStiffness() + k0 < 0) k0 = 0.0f;
		if (wheel[i1].suspension.getStiffness() + k1 < 0) k1 = 0.0f;

		wheel[i0].setAntiRollStiffness(k0);
		wheel[i1].setAntiRollStiffness(k1);
	}

	// wheel contacts
	int mcount = 0;
	int wcount = 0;
	abs_active = false;
	tcs_active = false;
	for (int i = 0; i < wheel.size(); ++i)
	{
		if (wheel[i].updateContact(dt, wheel_contact[wcount]))
		{
			wheel_contact[wcount].id = i;

			MotorJoint & joint = motor_joint[mcount];
			joint.shaft = &wheel[i].shaft;
			joint.impulseLimit = 0;
			joint.targetVelocity = wheel_contact[wcount].v1 / wheel[i].getRadius();
			joint.accumulatedImpulse = 0;
			abs_active |= wheel[i].getABS();
			tcs_active |= wheel[i].getTCS();
			mcount++;
			wcount++;
		}
	}

	// engine
	MotorJoint & joint = motor_joint[mcount];
	joint.shaft = &engine.getShaft();
	joint.targetVelocity = engine.getTorque() > 0 ? engine.getRPMLimit() * M_PI / 30 : 0;
	joint.impulseLimit = btFabs(engine.getTorque()) * dt;
	joint.accumulatedImpulse = 0;
	mcount++;

	// brakes
	for (int i = 0; i < wheel.size(); ++i)
	{
		btScalar torque = wheel[i].brake.getTorque();
		if (torque > 0)
		{
			MotorJoint & joint = motor_joint[mcount];
			joint.shaft = &wheel[i].shaft;
			joint.targetVelocity = 0;
			joint.impulseLimit = torque * dt;
			joint.accumulatedImpulse = 0;
			mcount++;
		}
	}

	// solver loop
	const int iterations = 8;
	for (int n = 0; n < iterations; ++n)
	{
		// wheel
		for (int i = 0; i < wcount; ++i)
		{
			WheelContact & c = wheel_contact[i];
			Wheel & w = wheel[c.id];

			SolveConstraintRow(c.response, *c.bodyA, *c.bodyB, c.rA, c.rB);

			btScalar load = c.response.accumImpulse / dt;
			btVector3 friction = w.tire.getForce(load, c.frictionCoeff, c.camber, c.vR, c.v1, c.v2);

			// lateral friction constraint
			if (friction[1] * dt > c.friction2.upperLimit)
			{
				c.friction2.upperLimit = friction[1] * dt;
			}
			else if (friction[1] * dt < c.friction2.lowerLimit)
			{
				c.friction2.lowerLimit = friction[1] * dt;
			}
			//clog << friction[0] << " " << w.tire.getSlide() << "; ";

			// tire friction torque
			btScalar impulseLimit = btFabs(friction[0]) * w.getRadius() * dt;
			if (impulseLimit > motor_joint[i].impulseLimit)
			{
				motor_joint[i].impulseLimit = impulseLimit;
			}
			motor_joint[i].targetVelocity = c.v1 / w.getRadius();
		}

		// driveline
		for (int i = 0; i < mcount; ++i)
		{
			motor_joint[i].solve();
		}
		for (int i = 0; i < dcount; ++i)
		{
			diff_joint[i].solve();
		}
		for (int i = 0; i < ccount; ++i)
		{
			clutch_joint[i].solve();
		}
		//clog << "  ";

		// wheel friction
		for (int i = 0; i < wcount; ++i)
		{
			WheelContact & c = wheel_contact[i];
			Wheel & w = wheel[c.id];

			// longitudinal friction costraint from tire friction torque
			btScalar impulseLimit = -motor_joint[i].accumulatedImpulse / w.getRadius();
			if (impulseLimit > c.friction1.upperLimit)
			{
				c.friction1.upperLimit = impulseLimit;
			}
			else if (impulseLimit < c.friction1.lowerLimit)
			{
				c.friction1.lowerLimit = impulseLimit;
			}
			//clog << -impulseLimit / w.getRadius() << " ";

			btScalar vel = w.shaft.getAngularVelocity() * w.getRadius();
			SolveConstraintRow(c.friction1, *c.bodyA, *c.bodyB, c.rA, c.rB, -vel);
			SolveConstraintRow(c.friction2, *c.bodyA, *c.bodyB, c.rA, c.rB);
		}
		//clog << "\n";
		//feedback += 0.5 * (wheel[0].tire.getFeedback() + wheel[1].tire.getFeedback()); fixme
	}
	//clog << "\n";
	//feedback /= (steps + 1);
}

void Vehicle::updateAerodynamics(btScalar dt)
{
	aero_force.setValue(0, 0, 0);
	aero_torque.setValue(0, 0, 0);
	const btMatrix3x3 inv = body->getCenterOfMassTransform().getBasis().transpose();
	btVector3 air_velocity = -(inv * body->getLinearVelocity());
	for (int i = 0; i < aero_device.size(); ++i)
	{
		btVector3 force = aero_device[i].getForce(air_velocity);
		btVector3 position = aero_device[i].getPosition() + body->getCenterOfMassOffset();
		aero_force = aero_force + force;
		aero_torque = aero_torque + position.cross(force);
	}
	btVector3 force = body->getCenterOfMassTransform().getBasis() * aero_force;
	btVector3 torque = body->getCenterOfMassTransform().getBasis() * aero_torque;
	body->applyCentralImpulse(force * dt);
	body->applyTorqueImpulse(torque * dt);
}

void Vehicle::updateWheelTransform(btScalar dt)
{
	for (int i = 0; i < wheel.size(); ++i)
	{
		if (!body->isChildConnected(i)) continue;

		wheel[i].shaft.integrate(dt);
		btQuaternion rot = wheel[i].suspension.getOrientation();
		rot *= btQuaternion(direction::right, -wheel[i].shaft.getAngle());
		btVector3 pos = wheel[i].suspension.getPosition() + body->getCenterOfMassOffset();
		body->setChildTransform(i, btTransform(rot, pos));
	}
}

void Vehicle::updateTransmission(btScalar dt)
{
	btScalar clutch_rpm = transmission.getClutchRPM();

	if (autoshift)
	{
		int gear = getNextGear(clutch_rpm);
		setGear(gear);
	}

	remaining_shift_time -= dt;
	if (remaining_shift_time < 0)
	{
		remaining_shift_time = 0;
	}

	if (!shifted && remaining_shift_time <= transmission.getShiftTime() * 0.5f)
	{
		transmission.shift(shift_gear);
		shifted = true;
	}

	if (autoclutch)
	{
		if (!engine.getCombustion())
		{
		    engine.start();
		}

		btScalar throttle = engine.getThrottle();
		throttle = autoClutchThrottle(clutch_rpm, throttle, dt);
		engine.setThrottle(throttle);

		btScalar new_clutch = autoClutch(clutch_rpm, last_clutch, dt);
		clutch.setPosition(new_clutch);
	}
	last_clutch = clutch.getPosition();
}

btScalar Vehicle::autoClutch(btScalar clutch_rpm, btScalar last_clutch, btScalar dt) const
{
//	clog << "last: " << last_clutch;

	// limit clutch load to keep engine rpm above stall
	btScalar clutch_value = 1;
	btScalar rpm_min = engine.getStartRPM();
	btScalar rpm_clutch = transmission.getClutchRPM();
	if (rpm_clutch < rpm_min)
	{
		// this seems to be pretty much binary(on, off)
		btScalar rpm = engine.getRPM();
		btScalar inertia = engine.getShaft().getInertia();
		btScalar torque_limit = inertia * (rpm - rpm_min) / dt;
		if (torque_limit > engine.getTorque())
		{
			torque_limit = engine.getTorque();
		}
		clutch_value = torque_limit / clutch.getTorqueMax();
//		clog << " stall: " << clutch_value;

		btClamp(clutch_value, btScalar(0), btScalar(1));
	}
//	clog << " cstall: " << clutch_value;

	// shift gear
	const btScalar shift_time = transmission.getShiftTime();
	if (remaining_shift_time > shift_time * 0.5)
	    clutch_value = 0.0;
	else if (remaining_shift_time > 0.0)
	    clutch_value *= (1.0 - remaining_shift_time / (shift_time * 0.5));
//	clog << " shift: " << clutch_value;

	// declutch when braking
	if (brake_value > 1E-3)
		clutch_value = 0.0;

	// rate limit the autoclutch
	btScalar engage_limit = 10 * dt;	// 10 steps
	btScalar clutch_delta = clutch_value - last_clutch;
	btClamp(clutch_delta, -engage_limit, engage_limit);
	clutch_value = last_clutch + clutch_delta;
//	clog << " limit: " << clutch_value;
//	clog << " rpmc: " << rpm_clutch << " rpme: " << engine.getRPM() << "\n";

	return clutch_value;
}

btScalar Vehicle::autoClutchThrottle(btScalar clutch_rpm, btScalar throttle, btScalar dt)
{
	if (engine.getRPM() < engine.getStartRPM() &&
		throttle < engine.getIdleThrottle())
	{
		// avoid stall
		throttle = engine.getIdleThrottle();
	}

	if (remaining_shift_time > 0.0)
	{
		// try to match clutch rpm
		const btScalar current_rpm = engine.getRPM();
		if (current_rpm < clutch_rpm && current_rpm < engine.getRedline())
		{
			remaining_shift_time += dt;
			throttle = 1.0;
		}
		else
		{
			throttle = 0.5 * throttle;
		}
	}

	return throttle;
}

int Vehicle::getNextGear(btScalar clutch_rpm) const
{
	int gear = transmission.getGear();

	// only autoshift if a shift is not in progress
	if (shifted && clutch.getPosition() == 1.0)
	{
		// shift up when driveshaft speed exceeds engine redline
		// we do not shift up from neutral/reverse
		if (clutch_rpm > engine.getRedline() && gear > 0)
		{
			return gear + 1;
		}
		// shift down when driveshaft speed below shift_down_point
		// we do not auto shift down from 1st gear to neutral
		if (clutch_rpm < getDownshiftRPM(gear) && gear > 1)
		{
			return gear - 1;
		}
	}
	return gear;
}

btScalar Vehicle::getDownshiftRPM(int gear) const
{
	// target rpm is 70% redline in the next lower gear
	btScalar shift_down_point = 0.0;
	if (gear > 1)
	{
		btScalar current_gear_ratio = transmission.getGearRatio(gear);
		btScalar lower_gear_ratio = transmission.getGearRatio(gear - 1);
		btScalar peak_engine_speed = engine.getRedline();
		shift_down_point = 0.7 * peak_engine_speed / lower_gear_ratio * current_gear_ratio;
	}
	return shift_down_point;
}

btScalar Vehicle::calculateMaxSpeed() const
{
	btScalar maxspeed = 250.0f / 3.6f; // fixme
	return maxspeed;
}

void Vehicle::calculateFrictionCoefficient(btScalar & lon_mu, btScalar & lat_mu) const
{
	btScalar lon_friction_factor = 0.68;
	btScalar lat_friction_factor = 0.62;
	btScalar gravity = 9.81;
	btScalar mass = 1 / body->getInvMass();
	btScalar force = gravity * mass / wheel.size();

	btScalar lon_friction = 0.0;
	btScalar lat_friction = 0.0;
	for (int i = 0; i < wheel.size(); ++i)
	{
		lon_friction += wheel[i].tire.getMaxFx(force);
		lat_friction += wheel[i].tire.getMaxFy(force, 0.0);
	}
	lon_friction /= force;
	lat_friction /= force;

	lon_mu = lon_friction_factor * lon_friction;
	lat_mu = lat_friction_factor * lat_friction;
}

static inline std::ostream & operator << (std::ostream & os, const btVector3 & v)
{
	os << v[0] << " " << v[1] << " " << v[2];
	return os;
}

void Vehicle::print(std::ostream & out, bool p1, bool p2, bool p3, bool p4) const
{
	const btScalar freq = 90;	// hack to get the units right
	out << std::fixed << std::setprecision(3);

	if (p1)
	{
		out << "\n\n\n\n\n\n\n";
		out << "---Body---\n";
		out << "Velocity: " << body->getLinearVelocity() << "\n";
		out << "Position: " << body->getCenterOfMassPosition() << "\n";
		out << "Center of mass: " << -body->getCenterOfMassOffset() << "\n";
		out << "Total mass: " << 1 / body->getInvMass() << "\n";
		out << "\n";
	}

	if (p2)
	{
		out << "\n\n\n\n\n\n\n";
		out << "---Engine---\n";
		out << "RPM: " << engine.getRPM() << "\n";
		out << "Power: " << engine.getTorque() * engine.getAngularVelocity() * 0.001 << "\n";
		out << "\n";

		int n = differential.size();
		out << "---Transmission---\n";
		out << "Clutch: " << last_clutch << "\n";
		out << "Gear Ratio: " << clutch_joint[n].gearRatio << "\n";
		out << "Engine Load: " << clutch_joint[n].accumulatedImpulse * freq * clutch_joint[n].shaft1->getAngularVelocity() * 0.001 << "\n";
		out << "Drive Load: " << -clutch_joint[n].accumulatedImpulse * freq * clutch_joint[n].gearRatio * clutch_joint[n].shaft2->getAngularVelocity() * 0.001 << "\n";
		out << "\n";

		for (int i = 0; i < n; ++i)
		{
			out << "---Differential---\n";
			out << "Gear Ratio: " << diff_joint[i].gearRatio << "\n";
			out << "Shaft RPM: " <<  diff_joint[i].shaft1->getAngularVelocity() * 30 / M_PI << "\n";
			out << "Shaft Load: " << diff_joint[i].accumulatedImpulse * freq * diff_joint[i].shaft1->getAngularVelocity() * 0.001 << "\n";
			out << "Shaft 1 Load: " << -diff_joint[i].accumulatedImpulse * freq * 0.5 * diff_joint[i].gearRatio * diff_joint[i].shaft2a->getAngularVelocity() * 0.001 << "\n";
			out << "Shaft 2 Load: " << -diff_joint[i].accumulatedImpulse * freq * 0.5 * diff_joint[i].gearRatio * diff_joint[i].shaft2b->getAngularVelocity() * 0.001 << "\n";
			out << "\n";
		}
	}

	if (p3)
	{
		out << "\n\n\n\n\n\n\n";
		int n = 0;
		for (int i = 0; i < wheel.size(); ++i)
		{
			out << "---Wheel---\n";
			out << "Travel: " <<  wheel[i].suspension.getDisplacement() << "\n";
			out << "Fz: " <<  wheel_contact[i].response.accumImpulse * freq * 1E-3 << "\n";
			out << "Ideal Slip: " <<  wheel[i].tire.getIdealSlide() << "\n";
			out << "Slip: " <<  wheel[i].tire.getSlide() << "\n";
			out << "Slip Angle: " <<  wheel[i].tire.getSlip() << "\n";
			out << "RPM: " <<  wheel[i].shaft.getAngularVelocity() * 30 / M_PI << "\n";
			if (wheel_contact[i].response.accumImpulse > 1E-3)
			{
				out << "Friction Load: " << motor_joint[n].accumulatedImpulse * freq * motor_joint[n].shaft->getAngularVelocity() * 0.001 << "\n";
				++n;
			}
			else
			{
				out << "Friction Load: 0\n";
			}
			out << "\n";
		}
	}

	if (p4)
	{
		out << "\n\n\n\n\n\n\n";
		out << "---Aerodynamics---" << "\n";
		out << "Force: " << aero_force << "\n";
		out << "Torque: " << aero_torque << "\n";
		out << "Lift/Drag: " << aero_force.z() / aero_force.y() << "\n\n";
		for (int i = 0; i != aero_device.size(); ++i)
		{
			out << "---Aerodynamic Device---" << "\n";
			out << "Drag: " << aero_device[i].getDrag() << "\n";
			out << "Lift: " << aero_device[i].getLift() << "\n\n";
		}
	}
}

bool Vehicle::WheelContactCallback(
	btManifoldPoint& cp,
	const btCollisionObject* colObj0,
	int partId0,
	int index0,
	const btCollisionObject* colObj1,
	int partId1,
	int index1)
{
	// cars are fracture bodies, wheel is a cylinder shape
	if (colObj0->getInternalType() & CO_FRACTURE_TYPE)
	{
		const btCollisionShape* shape = colObj0->getCollisionShape();
		if (shape->getShapeType() == CYLINDER_SHAPE_PROXYTYPE)
		{
			// is contact within contact patch?
			const btCompoundShape* car = static_cast<const btCompoundShape*>(colObj0->getRootCollisionShape());
			const btCylinderShapeX* wheel = static_cast<const btCylinderShapeX*>(shape);
			btVector3 contactPoint = cp.m_localPointA - car->getChildTransform(cp.m_index0).getOrigin();
			if (-direction::up.dot(contactPoint) > 0.5 * wheel->getRadius())
			{
				// break contact (hack)
				cp.m_normalWorldOnB = btVector3(0, 0, 0);
				cp.m_distance1 = 0;
				cp.m_combinedFriction = 0;
				cp.m_combinedRestitution = 0;
				return true;
			}
		}
	}
	return false;
}

}
