/*
 * Copyright (C) 2023 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <string>

#include <gz/common/Console.hh>
#include <gz/common/MouseEvent.hh>
#include <gz/gui/Application.hh>
#include <gz/gui/GuiEvents.hh>
#include <gz/gui/Helpers.hh>
#include <gz/gui/MainWindow.hh>
#include <gz/math/Inertial.hh>
#include <gz/math/PID.hh>
#include <gz/math/Pose3.hh>
#include <gz/math/Plane.hh>
#include <gz/math/Quaternion.hh>
#include <gz/math/Vector2.hh>
#include <gz/math/Vector3.hh>
#include <gz/msgs/entity_plugin_v.pb.h>
#include <gz/msgs/entity_wrench.pb.h>
#include <gz/msgs/Utility.hh>
#include <gz/plugin/Register.hh>
#include <gz/rendering/ArrowVisual.hh>
#include <gz/rendering/Camera.hh>
#include <gz/rendering/RayQuery.hh>
#include <gz/rendering/RenderingIface.hh>
#include <gz/rendering/RenderTypes.hh>
#include <gz/rendering/Scene.hh>
#include <gz/rendering/Utils.hh>
#include <gz/sim/Entity.hh>
#include <gz/sim/EntityComponentManager.hh>
#include <gz/sim/Link.hh>
#include <gz/sim/Util.hh>
#include <gz/sim/components/Inertial.hh>
#include <gz/sim/components/Name.hh>
#include <gz/sim/components/ParentEntity.hh>
#include <gz/sim/components/SystemPluginInfo.hh>
#include <gz/sim/components/World.hh>
#include <gz/sim/gui/GuiEvents.hh>
#include <gz/transport/Node.hh>

#include "MouseDrag.hh"

namespace gz
{
namespace sim
{
  /// \enum MouseDragMode
  /// \brief Unique identifiers for mouse dragging modes
  enum MouseDragMode
  {
    /// \brief Inactive state
    NONE = 0,
    /// \brief Rotation mode
    ROTATE = 1,
    /// \brief Translation mode
    TRANSLATE = 2,
  };

  class MouseDragPrivate
  {
    /// \brief Handle mouse events
    public: void HandleMouseEvents();

    /// \brief Perform rendering calls in the rendering thread.
    public: void OnRender();

    /// \brief Calculate the wrench to be applied to the link,
    /// based on a spring-damper control law.
    ///
    /// This attempts to minimize the error (in position or
    /// rotation, depending on the mode) with a proportional control, while
    /// damping the rate of change of the link's pose.
    ///
    /// \param[in] _error Position error in translation mode, Rotation error
    /// (in Euler angles) in rotation mode
    /// \param[in] _dt Change in time since last call
    /// \param[in] _linkWorldPose World pose of the link
    /// \param[in] _inertial inertial of the link
    /// \return The EntityWrench message containing the calculated
    /// force and torque
    public: msgs::EntityWrench CalculateWrench(
      const math::Vector3d &_error,
      const std::chrono::duration<double> &_dt,
      const math::Pose3d &_linkWorldPose,
      const math::Inertiald &_inertial);

    /// \brief Transport node
    public: transport::Node node;

    /// \brief Publisher for EntityWrench messages
    public: transport::Node::Publisher pub;

    /// \brief World name
    public: std::string worldName;

    /// \brief Pointer to the rendering scene
    public: rendering::ScenePtr scene{nullptr};

    /// \brief User camera
    public: rendering::CameraPtr camera{nullptr};

    /// \brief Ray query for mouse clicks
    public: rendering::RayQueryPtr rayQuery{nullptr};

    /// \brief Holds the latest mouse event
    public: gz::common::MouseEvent mouseEvent;

    /// \brief Initial position of a mouse click
    public: math::Vector2i mousePressPos;

    /// \brief True if there are new mouse events to process
    public: bool mouseDirty{false};

    /// \brief True if there are new dragging mode changes to process
    public: bool modeDirty{false};

    /// \brief True if the force should be applied to the center of mass
    public: bool applyCOM{false};

    /// \brief True if camera orbit should be blocked
    public: bool blockOrbit{false};

    /// \brief True if BlockOrbit events should be sent
    public: bool sendBlockOrbit{false};

    /// \brief True if dragging is active
    public: bool dragActive{false};

    /// \brief True if the ApplyLinkWrench system is loaded
    public: bool systemLoaded{false};

    /// \brief Current dragging mode
    public: MouseDragMode mode = MouseDragMode::NONE;

    /// \brief Link to which the wrenches are applied
    public: Entity linkId{kNullEntity};

    /// \brief Plane of force application
    public: math::Planed plane;

    /// \brief Application point of the wrench in world coordinates
    public: math::Vector3d applicationPoint;

    /// \brief Initial world rotation of the link during mouse click
    public: math::Quaterniond initialRot;

    /// \brief Point to which the link is dragged in translation mode
    math::Vector3d target;

    /// \brief Goal link rotation for rotation mode
    public: math::Quaterniond goalRot;

    /// \brief Offset of the force application point relative to the
    /// link origin, expressed in the link-fixed frame
    public: math::Vector3d offset{0.0, 0.0, 0.0};

    /// \brief Link world pose in previous Update call. Used for damping
    public: math::Pose3d poseLast;

    /// \brief Spring stiffness for translation, in (m/s²)/m
    public: double posStiffness = 100.0;

    /// \brief Spring stiffness for rotation, in (rad/s²)/rad
    public: double rotStiffness = 200.0;

    /// \brief Arrow for visualizing force in translation mode.
    /// This arrow goes from the application point to the target point.
    public: rendering::ArrowVisualPtr arrowVisual{nullptr};

    /// \brief Box for visualizing rotation mode
    public: rendering::VisualPtr boxVisual{nullptr};

    /// \brief Size of the bounding box of the selected link
    public: math::Vector3d bboxSize;
  };
}
}

using namespace gz;
using namespace sim;

/////////////////////////////////////////////////
MouseDrag::MouseDrag()
  : GuiSystem(), dataPtr(std::make_unique<MouseDragPrivate>())
{
}

/////////////////////////////////////////////////
MouseDrag::~MouseDrag() = default;

/////////////////////////////////////////////////
void MouseDrag::LoadConfig(const tinyxml2::XMLElement *_pluginElem)
{
  if (this->title.empty())
    this->title = "Mouse drag";

  // Create wrench publisher
  auto worldNames = gz::gui::worldNames();
  if (!worldNames.empty())
  {
    this->dataPtr->worldName = worldNames[0].toStdString();
    auto topic = transport::TopicUtils::AsValidTopic(
      "/world/" + this->dataPtr->worldName + "/wrench");
    if (topic.empty())
    {
      gzerr << "Unable to create publisher" << std::endl;
      return;
    }
    this->dataPtr->pub =
      this->dataPtr->node.Advertise<msgs::EntityWrench>(topic);
    gzdbg << "Created publisher to " << topic << std::endl;
  }

  // Read configuration
  if (_pluginElem)
  {
    if (auto elem = _pluginElem->FirstChildElement("rotation_stiffness"))
    {
      elem->QueryDoubleText(&this->dataPtr->rotStiffness);
      emit this->RotStiffnessChanged();
    }

    if (auto elem = _pluginElem->FirstChildElement("position_stiffness"))
    {
      elem->QueryDoubleText(&this->dataPtr->posStiffness);
      emit this->PosStiffnessChanged();
    }
  }

  gz::gui::App()->findChild<gz::gui::MainWindow *>
    ()->installEventFilter(this);
}

/////////////////////////////////////////////////
bool MouseDrag::eventFilter(QObject *_obj, QEvent *_event)
{
  if (_event->type() == gz::gui::events::Render::kType)
  {
    this->dataPtr->OnRender();
  }
  else if (_event->type() == gz::gui::events::LeftClickOnScene::kType)
  {
    auto event =
      static_cast<gz::gui::events::LeftClickOnScene *>(_event);
    this->dataPtr->mouseEvent = event->Mouse();
    this->dataPtr->mouseDirty = true;
  }
  else if (_event->type() == gz::gui::events::RightClickOnScene::kType)
  {
    auto event =
      static_cast<gz::gui::events::RightClickOnScene *>(_event);
    this->dataPtr->mouseEvent = event->Mouse();
    this->dataPtr->mouseDirty = true;
  }
  else if (_event->type() == gz::gui::events::MousePressOnScene::kType)
  {
    auto event =
      static_cast<gz::gui::events::MousePressOnScene *>(_event);
    this->dataPtr->mouseEvent = event->Mouse();
    this->dataPtr->mouseDirty = true;
  }
  else if (_event->type() == gz::gui::events::DragOnScene::kType)
  {
    auto event =
      static_cast<gz::gui::events::DragOnScene *>(_event);
    this->dataPtr->mouseEvent = event->Mouse();
    this->dataPtr->mouseDirty = true;
  }

  this->dataPtr->HandleMouseEvents();

  return QObject::eventFilter(_obj, _event);
}

/////////////////////////////////////////////////
void MouseDrag::Update(const UpdateInfo &_info,
  EntityComponentManager &_ecm)
{
  // Load the ApplyLinkWrench system
  if (!this->dataPtr->systemLoaded)
  {
    const std::string name{"gz::sim::systems::ApplyLinkWrench"};
    const std::string filename{"gz-sim-apply-link-wrench-system"};
    const std::string innerxml{"<verbose>0</verbose>"};

    // Get world entity
    Entity worldEntity;
    _ecm.Each<components::World, components::Name>(
      [&](const Entity &_entity,
        const components::World */*_world*/,
        const components::Name *_name)->bool
      {
        if (_name->Data() == this->dataPtr->worldName)
        {
          worldEntity = _entity;
          return false;
        }
        return true;
      });

    // Check if already loaded
    auto msg = _ecm.ComponentData<components::SystemPluginInfo>(worldEntity);
    if (!msg)
    {
      gzdbg << "Unable to find SystemPluginInfo component for entity "
            << worldEntity << std::endl;
      return;
    }
    for (const auto &plugin : msg->plugins())
    {
      if (plugin.filename() == filename)
      {
        this->dataPtr->systemLoaded = true;
        gzdbg << "ApplyLinkWrench system already loaded" << std::endl;
        break;
      }
    }

    // Request to load system
    if (!this->dataPtr->systemLoaded)
    {
      msgs::EntityPlugin_V req;
      req.mutable_entity()->set_id(worldEntity);
      auto plugin = req.add_plugins();
      plugin->set_name(name);
      plugin->set_filename(filename);
      plugin->set_innerxml(innerxml);

      msgs::Boolean res;
      bool result;
      unsigned int timeout = 5000;
      auto service = transport::TopicUtils::AsValidTopic(
        "/world/" + this->dataPtr->worldName + "/entity/system/add");
      if (!service.empty() &&
          this->dataPtr->node.Request(service, req, timeout, res, result))
      {
        this->dataPtr->systemLoaded = true;
        gzdbg << "ApplyLinkWrench system has been loaded" << std::endl;
      }
      else
      {
        gzerr << "Error adding new system to entity: "
              << worldEntity << "\n"
              << "Name: " << name << "\n"
              << "Filename: " << filename << "\n"
              << "Inner XML: " << innerxml << std::endl;
      }
    }
  }

  if (this->dataPtr->mode == MouseDragMode::NONE ||
      _info.paused)
  {
    this->dataPtr->mode = MouseDragMode::NONE;
    this->dataPtr->dragActive = false;
    return;
  }

  // Get Link corresponding to clicked Visual
  Link link(this->dataPtr->linkId);
  auto model = link.ParentModel(_ecm);
  if (!link.Valid(_ecm) || model->Static(_ecm))
  {
    this->dataPtr->blockOrbit = false;
    return;
  }

  auto linkWorldPose = worldPose(this->dataPtr->linkId, _ecm);
  auto inertial = _ecm.Component<components::Inertial>(this->dataPtr->linkId);
  if (!inertial)
  {
    return;
  }

  if (this->dataPtr->blockOrbit)
    this->dataPtr->sendBlockOrbit = true;

  this->dataPtr->dragActive = true;

  if (this->dataPtr->modeDirty)
  {
    this->dataPtr->modeDirty = false;

    gui::events::DeselectAllEntities event(true);
    gz::gui::App()->sendEvent(
      gz::gui::App()->findChild<gz::gui::MainWindow *>(),
      &event);

    this->dataPtr->mousePressPos = this->dataPtr->mouseEvent.Pos();
    this->dataPtr->initialRot = linkWorldPose.Rot();
    this->dataPtr->poseLast = linkWorldPose;

    // Calculate offset of force application from link origin
    if (this->dataPtr->applyCOM || this->dataPtr->mode == MouseDragMode::ROTATE)
    {
      this->dataPtr->offset = inertial->Data().Pose().Pos();
      this->dataPtr->applicationPoint =
        linkWorldPose.Pos() +
        linkWorldPose.Rot().RotateVector(this->dataPtr->offset);
    }
    else
    {
      this->dataPtr->offset = linkWorldPose.Rot().RotateVectorReverse(
        this->dataPtr->applicationPoint - linkWorldPose.Pos());
    }

    // The plane of wrench application should be normal to the center
    // of the camera view and pass through the application point
    math::Vector3d direction = this->dataPtr->camera->WorldPose().Rot().XAxis();
    double planeOffset = direction.Dot(this->dataPtr->applicationPoint);
    this->dataPtr->plane = math::Planed(direction, planeOffset);
  }
  // Track the application point
  else
  {
    this->dataPtr->applicationPoint =
      linkWorldPose.Pos() +
      linkWorldPose.Rot().RotateVector(this->dataPtr->offset);
  }

  // Normalize mouse position on the image
  double width = this->dataPtr->camera->ImageWidth();
  double height = this->dataPtr->camera->ImageHeight();

  double nx = 2.0 * this->dataPtr->mouseEvent.Pos().X() / width - 1.0;
  double ny = 1.0 - 2.0 * this->dataPtr->mouseEvent.Pos().Y() / height;

  // Make a ray query at the mouse position
  this->dataPtr->rayQuery->SetFromCamera(
    this->dataPtr->camera, math::Vector2d(nx, ny));
  math::Vector3d end = this->dataPtr->rayQuery->Direction();

  // Wrench in world coordinates, applied at the link origin
  msgs::EntityWrench msg;
  if (this->dataPtr->mode == MouseDragMode::ROTATE)
  {
    // Calculate rotation angle from mouse displacement
    nx = 2.0 * this->dataPtr->mousePressPos.X() / width - 1.0;
    ny = 1.0 - 2.0 * this->dataPtr->mousePressPos.Y() / height;
    this->dataPtr->rayQuery->SetFromCamera(
      this->dataPtr->camera, math::Vector2d(nx, ny));
    math::Vector3d start = this->dataPtr->rayQuery->Direction();
    math::Vector3d axis = start.Cross(end);
    double angle = -atan2(axis.Length(), start.Dot(end));

    // Project rotation axis onto plane
    axis -= axis.Dot(this->dataPtr->plane.Normal()) *
      this->dataPtr->plane.Normal();
    axis.Normalize();

    // Calculate the necessary rotation and torque
    this->dataPtr->goalRot =
      math::Quaterniond(axis, angle) * this->dataPtr->initialRot;
    math::Quaterniond errorRot =
      linkWorldPose.Rot() * this->dataPtr->goalRot.Inverse();
    msg = this->dataPtr->CalculateWrench(
      errorRot.Euler(), _info.dt, linkWorldPose, inertial->Data());
  }
  else if (this->dataPtr->mode == MouseDragMode::TRANSLATE)
  {
    math::Vector3d origin = this->dataPtr->rayQuery->Origin();
    if (auto t = this->dataPtr->plane.Intersection(origin, end))
      this->dataPtr->target = *t;
    else
      return;

    math::Vector3d errorPos =
      this->dataPtr->applicationPoint - this->dataPtr->target;
    msg = this->dataPtr->CalculateWrench(
      errorPos, _info.dt, linkWorldPose, inertial->Data());
  }

  // Publish wrench
  this->dataPtr->pub.Publish(msg);
}

/////////////////////////////////////////////////
void MouseDrag::OnSwitchCOM(const bool _checked)
{
  this->dataPtr->applyCOM = _checked;
}

/////////////////////////////////////////////////
double MouseDrag::RotStiffness() const
{
  return this->dataPtr->rotStiffness;
}

/////////////////////////////////////////////////
void MouseDrag::SetRotStiffness(double _rotStiffness)
{
  this->dataPtr->rotStiffness = _rotStiffness;
}

/////////////////////////////////////////////////
double MouseDrag::PosStiffness() const
{
  return this->dataPtr->posStiffness;
}

/////////////////////////////////////////////////
void MouseDrag::SetPosStiffness(double _posStiffness)
{
  this->dataPtr->posStiffness = _posStiffness;
}

/////////////////////////////////////////////////
void MouseDragPrivate::OnRender()
{
  // Get scene and user camera
  if (!this->scene)
  {
    this->scene = rendering::sceneFromFirstRenderEngine();
    if (!this->scene)
    {
      return;
    }

    for (unsigned int i = 0; i < this->scene->NodeCount(); ++i)
    {
      auto cam = std::dynamic_pointer_cast<rendering::Camera>(
        this->scene->NodeByIndex(i));
      if (cam && cam->HasUserData("user-camera") &&
          std::get<bool>(cam->UserData("user-camera")))
      {
        this->camera = cam;
        gzdbg << "MouseDrag plugin is using camera ["
              << this->camera->Name() << "]" << std::endl;
        break;
      }
    }

    if (!this->camera)
    {
      gzerr << "MouseDrag camera is not available" << std::endl;
      return;
    }
    this->rayQuery = this->scene->CreateRayQuery();

    auto mat = this->scene->Material("Default/TransRed")->Clone();
    mat->SetDepthCheckEnabled(false);
    mat->SetDepthWriteEnabled(false);

    this->arrowVisual = this->scene->CreateArrowVisual();
    this->arrowVisual->SetMaterial(mat);
    this->arrowVisual->ShowArrowHead(true);
    this->arrowVisual->ShowArrowShaft(true);
    this->arrowVisual->ShowArrowRotation(false);
    this->arrowVisual->SetVisible(false);

    this->boxVisual = scene->CreateVisual();
    this->boxVisual->AddGeometry(scene->CreateBox());
    this->boxVisual->SetInheritScale(false);
    this->boxVisual->SetMaterial(mat);
    this->boxVisual->SetUserData("gui-only", static_cast<bool>(true));
    this->boxVisual->SetVisible(false);
  }

  // Update the visualization
  if (!this->dragActive)
  {
    this->arrowVisual->SetVisible(false);
    this->boxVisual->SetVisible(false);
  }
  else if (this->mode == MouseDragMode::ROTATE)
  {
    this->boxVisual->SetLocalPosition(this->applicationPoint);
    this->boxVisual->SetLocalRotation(this->goalRot);
    this->boxVisual->SetLocalScale(1.2 * this->bboxSize);

    this->arrowVisual->SetVisible(false);
    this->boxVisual->SetVisible(true);
  }
  else if (this->mode == MouseDragMode::TRANSLATE)
  {
    math::Vector3d axisDir = this->target - this->applicationPoint;
    math::Vector3d u = axisDir.Normalized();
    math::Vector3d v = gz::math::Vector3d::UnitZ;
    double angle = acos(v.Dot(u));
    math::Quaterniond quat;
    // check the parallel case
    if (math::equal(angle, GZ_PI))
      quat.SetFromAxisAngle(u.Perpendicular(), angle);
    else
      quat.SetFromAxisAngle((v.Cross(u)).Normalize(), angle);
    double scale = 2 * this->bboxSize.Length();

    this->arrowVisual->SetLocalPosition(this->applicationPoint);
    this->arrowVisual->SetLocalRotation(quat);
    this->arrowVisual->SetLocalScale(scale, scale, axisDir.Length() / 0.75);

    this->arrowVisual->SetVisible(true);
    this->boxVisual->SetVisible(false);
  }

  if (this->sendBlockOrbit)
  {
    // Events with false should only be sent once
    if (!this->blockOrbit)
      this->sendBlockOrbit = false;

    gz::gui::events::BlockOrbit blockOrbitEvent(this->blockOrbit);
    gz::gui::App()->sendEvent(
        gz::gui::App()->findChild<gz::gui::MainWindow *>(),
        &blockOrbitEvent);
  }
}

/////////////////////////////////////////////////
void MouseDragPrivate::HandleMouseEvents()
{
  // Check for mouse events
  if (!this->mouseDirty)
  {
    return;
  }
  this->mouseDirty = false;

  if (this->mouseEvent.Type() == common::MouseEvent::PRESS &&
      this->mouseEvent.Control() &&
      this->mouseEvent.Button() != common::MouseEvent::MIDDLE)
  {
    // Get the visual at mouse position
    rendering::VisualPtr visual = this->scene->VisualAt(
      this->camera,
      this->mouseEvent.Pos());

    if (!visual)
    {
      this->mode = MouseDragMode::NONE;
      return;
    }
    try
    {
      this->linkId =
        std::get<uint64_t>(visual->Parent()->UserData("gazebo-entity"));
    }
    catch(std::bad_variant_access &e)
    {
      this->mode = MouseDragMode::NONE;
      return;
    }

    this->blockOrbit = true;
    this->modeDirty = true;

    // Get the 3D coordinates of the clicked point
    this->applicationPoint = rendering::screenToScene(
      this->mouseEvent.Pos(), this->camera,
      this->rayQuery);

    this->bboxSize = visual->LocalBoundingBox().Size();

    if (this->mouseEvent.Button() == common::MouseEvent::LEFT)
    {
      this->mode = MouseDragMode::ROTATE;
    }
    else if (this->mouseEvent.Button() == common::MouseEvent::RIGHT)
    {
      this->mode = MouseDragMode::TRANSLATE;
    }
  }
  else if (this->mouseEvent.Type() == common::MouseEvent::RELEASE)
  {
    this->mode = MouseDragMode::NONE;
    this->dragActive = false;
    this->blockOrbit = false;
  }
  else if (this->mouseEvent.Type() == common::MouseEvent::MOVE)
  {
    if (this->mode != MouseDragMode::NONE)
    {
      this->blockOrbit = true;
    }
  }
}

/////////////////////////////////////////////////
msgs::EntityWrench MouseDragPrivate::CalculateWrench(
  const math::Vector3d &_error,
  const std::chrono::duration<double> &_dt,
  const math::Pose3d &_linkWorldPose,
  const math::Inertiald &_inertial)
{
  math::Vector3d force, torque;
  if (this->mode == MouseDragMode::ROTATE)
  {
    // TODO(anyone): is this the best way to scale rotation gains?
    double avgInertia = _inertial.MassMatrix().PrincipalMoments().Sum() / 3;

    math::Vector3d dErrorRot =
      (_linkWorldPose.Rot() * this->poseLast.Rot().Inverse()).Euler()
      / _dt.count();
    for (auto i : {0, 1, 2})
    {
      double pGainRot = this->rotStiffness * avgInertia;
      double dGainRot = 2 * sqrt(this->rotStiffness) * avgInertia;

      // Correct angle discontinuity around 0/2pi
      if (dErrorRot[i] > GZ_PI)
        dErrorRot[i] -= 2 * GZ_PI;
      else if (dErrorRot[i] < -GZ_PI)
        dErrorRot[i] += 2 * GZ_PI;

      torque[i] = - pGainRot * _error[i] - dGainRot * dErrorRot[i];
    }
  }
  else if (this->mode == MouseDragMode::TRANSLATE)
  {
    double mass = _inertial.MassMatrix().Mass();
    double pGainPos = this->posStiffness * mass;
    double dGainPos = 0.5 * sqrt(this->posStiffness) * mass;

    math::Vector3d dErrorPos =
      (_linkWorldPose.Pos() - this->poseLast.Pos()) / _dt.count();

    force = - pGainPos * _error - dGainPos * dErrorPos;
    torque =
      _linkWorldPose.Rot().RotateVector(this->offset).Cross(force);

    // If the force is not applied to the center of mass, slightly damp the
    // resulting rotation
    if (!this->applyCOM)
    {
      double avgInertia = _inertial.MassMatrix().PrincipalMoments().Sum() / 3;

      math::Vector3d dErrorRot =
        (_linkWorldPose.Rot() * this->poseLast.Rot().Inverse()).Euler()
        / _dt.count();
      for (auto i : {0, 1, 2})
      {
        double dGainRot = 0.5 * sqrt(this->rotStiffness) * avgInertia;

        // Correct angle discontinuity around 0/2pi
        if (dErrorRot[i] > GZ_PI)
          dErrorRot[i] -= 2 * GZ_PI;
        else if (dErrorRot[i] < -GZ_PI)
          dErrorRot[i] += 2 * GZ_PI;

        torque -= dGainRot * dErrorRot[i];
      }
    }
  }
  else
  {
    return msgs::EntityWrench();
  }

  this->poseLast = _linkWorldPose;

  msgs::EntityWrench msg;
  msg.mutable_entity()->set_id(this->linkId);
  msgs::Set(msg.mutable_wrench()->mutable_force(), force);
  msgs::Set(msg.mutable_wrench()->mutable_torque(), torque);
  return msg;
}

// Register this plugin
GZ_ADD_PLUGIN(MouseDrag, gz::gui::Plugin);
