/* Copyright (c) 2016 - 2023, the adamantine authors.
 *
 * This file is subject to the Modified BSD License and may not be distributed
 * without copyright and license information. Please refer to the file LICENSE
 * for the text and further information on this license.
 */

#ifndef THERMAL_PHYSICS_TEMPLATES_HH
#define THERMAL_PHYSICS_TEMPLATES_HH

#include <ThermalOperator.hh>

#include <deal.II/base/index_set.h>
#include <deal.II/lac/read_write_vector.h>
#if defined(ADAMANTINE_HAVE_CUDA) && defined(__CUDACC__)
#include <ThermalOperatorDevice.hh>
#endif
#include <CubeHeatSource.hh>
#include <ElectronBeamHeatSource.hh>
#include <GoldakHeatSource.hh>
#include <ThermalPhysics.hh>
#include <Timer.hh>

#include <deal.II/base/geometry_info.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/distributed/cell_data_transfer.templates.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/fe_nothing.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/grid/filtered_iterator.h>
#include <deal.II/hp/fe_values.h>
#include <deal.II/hp/q_collection.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/lac/solver_gmres.h>
#include <deal.II/lac/vector_operation.h>

#ifdef ADAMANTINE_WITH_CALIPER
#include <caliper/cali.h>
#endif

#include <algorithm>
#include <memory>

namespace adamantine
{
namespace
{
template <int dim, int fe_degree, typename MemorySpaceType,
          std::enable_if_t<
              std::is_same<MemorySpaceType, dealii::MemorySpace::Host>::value,
              int> = 0>
dealii::LA::distributed::Vector<double, MemorySpaceType>
evaluate_thermal_physics_impl(
    std::shared_ptr<ThermalOperatorBase<dim, MemorySpaceType>> thermal_operator,
    double const t, double const current_source_height,
    dealii::LA::distributed::Vector<double, MemorySpaceType> const &y,
    std::vector<Timer> &timers)
{
  timers[evol_time_eval_th_ph].start();
  thermal_operator->set_time_and_source_height(t, current_source_height);

  dealii::LA::distributed::Vector<double, dealii::MemorySpace::Host> value(
      y.get_partitioner());
  value = 0.;
  // Apply the Thermal Operator.
  thermal_operator->vmult_add(value, y);

  // Multiply by the inverse of the mass matrix.
  value.scale(*thermal_operator->get_inverse_mass_matrix());

  timers[evol_time_eval_th_ph].stop();

  return value;
}

template <int dim, int fe_degree, typename MemorySpaceType,
          std::enable_if_t<
              std::is_same<MemorySpaceType, dealii::MemorySpace::Host>::value,
              int> = 0>
void init_dof_vector(
    double const value,
    dealii::LinearAlgebra::distributed::Vector<double, MemorySpaceType> &vector)
{
  unsigned int const local_size = vector.locally_owned_size();
  for (unsigned int i = 0; i < local_size; ++i)
    vector.local_element(i) = value;
}

#if defined(ADAMANTINE_HAVE_CUDA) && defined(__CUDACC__)
template <int dim, int fe_degree, typename MemorySpaceType,
          std::enable_if_t<
              std::is_same<MemorySpaceType, dealii::MemorySpace::CUDA>::value,
              int> = 0>
dealii::LA::distributed::Vector<double, MemorySpaceType>
evaluate_thermal_physics_impl(
    std::shared_ptr<ThermalOperatorBase<dim, MemorySpaceType>> const
        &thermal_operator,
    dealii::hp::FECollection<dim> const &fe_collection, double const t,
    dealii::DoFHandler<dim> const &dof_handler,
    std::vector<std::shared_ptr<HeatSource<dim>>> const &heat_sources,
    double current_source_height, BoundaryType boundary_type,
    MaterialProperty<dim, MemorySpaceType> &material_properties,
    dealii::AffineConstraints<double> const &affine_constraints,
    dealii::LA::distributed::Vector<double, MemorySpaceType> const &y,
    std::vector<Timer> &timers)
{
  auto thermal_operator_dev = std::dynamic_pointer_cast<
      ThermalOperatorDevice<dim, fe_degree, MemorySpaceType>>(thermal_operator);
  timers[evol_time_update_bound_mat_prop].start();
  thermal_operator_dev->update_boundary_material_properties(y);
  timers[evol_time_update_bound_mat_prop].stop();

  timers[evol_time_eval_th_ph].start();

  dealii::LA::distributed::Vector<double, MemorySpaceType> value_dev(
      y.get_partitioner());

  // Apply the Thermal Operator.
  thermal_operator_dev->vmult(value_dev, y);

  // Compute the source term.
  // TODO do this on the GPU
  for (auto &beam : heat_sources)
    beam->update_time(t);
  dealii::LA::distributed::Vector<double, dealii::MemorySpace::Host> source(
      y.get_partitioner());
  source = 0.;

  // Compute inv_rho_cp at the cell level on the host. We would not need to do
  // this if everything was done on the GPU.
  thermal_operator_dev->update_inv_rho_cp_cell();

  dealii::hp::QCollection<dim> source_q_collection;
  source_q_collection.push_back(dealii::QGauss<dim>(fe_degree + 1));
  source_q_collection.push_back(dealii::QGauss<dim>(1));
  dealii::hp::FEValues<dim> hp_fe_values(fe_collection, source_q_collection,
                                         dealii::update_quadrature_points |
                                             dealii::update_values |
                                             dealii::update_JxW_values);
  unsigned int const dofs_per_cell = fe_collection.max_dofs_per_cell();
  unsigned int const n_q_points = source_q_collection.max_n_quadrature_points();
  std::vector<dealii::types::global_dof_index> local_dof_indices(dofs_per_cell);
  dealii::QGauss<dim - 1> face_quadrature(fe_degree + 1);
  dealii::FEFaceValues<dim> fe_face_values(
      fe_collection[0], face_quadrature,
      dealii::update_values | dealii::update_quadrature_points |
          dealii::update_JxW_values);
  unsigned int const n_face_q_points = face_quadrature.size();
  dealii::Vector<double> cell_source(dofs_per_cell);

  // Loop over the locally owned cells with an active FE index of zero
  for (auto const &cell : dealii::filter_iterators(
           dof_handler.active_cell_iterators(),
           dealii::IteratorFilters::LocallyOwnedCell(),
           dealii::IteratorFilters::ActiveFEIndexEqualTo(0)))
  {
    cell_source = 0.;
    hp_fe_values.reinit(cell);
    dealii::FEValues<dim> const &fe_values =
        hp_fe_values.get_present_fe_values();

    for (unsigned int i = 0; i < dofs_per_cell; ++i)
    {
      for (unsigned int q = 0; q < n_q_points; ++q)
      {
        double const inv_rho_cp = thermal_operator_dev->get_inv_rho_cp(cell, q);
        double quad_pt_source = 0.;
        dealii::Point<dim> const &q_point = fe_values.quadrature_point(q);
        for (auto &beam : heat_sources)
          quad_pt_source += beam->value(q_point, current_source_height);

        cell_source[i] += inv_rho_cp * quad_pt_source *
                          fe_values.shape_value(i, q) * fe_values.JxW(q);
      }
    }

    // If we don't have a adiabatic boundary conditions, we need to add boundary
    // conditions
    if (!(boundary_type & BoundaryType::adiabatic))
    {
      for (unsigned int f = 0; f < dealii::GeometryInfo<dim>::faces_per_cell;
           ++f)
      {
        // We need to add the boundary conditions on the faces on the boundary
        // but also on the faces at the interface with FE_Nothing
        auto const &face = cell->face(f);
        if ((face->at_boundary()) &&
            ((!face->at_boundary()) &&
             (cell->neighbor(f)->active_fe_index() != 0)))
        {
          double conv_temperature_infty = 0.;
          double conv_heat_transfer_coef = 0.;
          double rad_temperature_infty = 0.;
          double rad_heat_transfer_coef = 0.;
          if (boundary_type & BoundaryType::convective)
          {
            conv_temperature_infty = material_properties.get_cell_value(
                cell, Property::convection_temperature_infty);
            conv_heat_transfer_coef = material_properties.get_cell_value(
                cell, StateProperty::convection_heat_transfer_coef);
          }
          if (boundary_type & BoundaryType::radiative)
          {
            rad_temperature_infty = material_properties.get_cell_value(
                cell, Property::radiation_temperature_infty);
            rad_heat_transfer_coef = material_properties.get_cell_value(
                cell, StateProperty::radiation_heat_transfer_coef);
          }

          fe_face_values.reinit(cell, face);
          for (unsigned int i = 0; i < dofs_per_cell; ++i)
          {
            for (unsigned int q = 0; q < n_face_q_points; ++q)
            {
              double const inv_rho_cp =
                  thermal_operator_dev->get_inv_rho_cp(cell, q);
              cell_source[i] +=
                  inv_rho_cp *
                  (conv_heat_transfer_coef * conv_temperature_infty +
                   rad_heat_transfer_coef * rad_temperature_infty) *
                  fe_face_values.shape_value(i, q) * fe_face_values.JxW(q);
            }
          }
        }
      }
    }
    cell->get_dof_indices(local_dof_indices);
    affine_constraints.distribute_local_to_global(cell_source,
                                                  local_dof_indices, source);
  }
  source.compress(dealii::VectorOperation::add);

  // Add source
  dealii::LA::distributed::Vector<double, dealii::MemorySpace::CUDA> source_dev(
      source.get_partitioner());
  source_dev.import(source, dealii::VectorOperation::insert);
  value_dev += source_dev;

  // Multiply by the inverse of the mass matrix.
  value_dev.scale(*thermal_operator_dev->get_inverse_mass_matrix());

  timers[evol_time_eval_th_ph].stop();

  return value_dev;
}

template <int dim, int fe_degree, typename MemorySpaceType,
          std::enable_if_t<
              std::is_same<MemorySpaceType, dealii::MemorySpace::CUDA>::value,
              int> = 0>
void init_dof_vector(
    double const value,
    dealii::LinearAlgebra::distributed::Vector<double, MemorySpaceType> &vector)
{
  dealii::LA::distributed::Vector<double, dealii::MemorySpace::Host>
      vector_host(vector.get_partitioner());
  unsigned int const local_size = vector_host.locally_owned_size();
  for (unsigned int i = 0; i < local_size; ++i)
    vector_host.local_element(i) = value;

  vector.import(vector_host, dealii::VectorOperation::insert);
}
#endif
} // namespace

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
ThermalPhysics<dim, fe_degree, MemorySpaceType, QuadratureType>::ThermalPhysics(
    MPI_Comm const &communicator, boost::property_tree::ptree const &database,
    Geometry<dim> &geometry,
    MaterialProperty<dim, MemorySpaceType> &material_properties)
    : _boundary_type(BoundaryType::invalid), _geometry(geometry),
      _dof_handler(_geometry.get_triangulation()),
      _cell_weights(
          _dof_handler,
          dealii::parallel::CellWeights<dim>::ndofs_weighting({1, 1})),
      _material_properties(material_properties)
{
  // Create the FECollection
  _fe_collection.push_back(dealii::FE_Q<dim>(fe_degree));
  _fe_collection.push_back(dealii::FE_Nothing<dim>());

  // Create the QCollection
  _q_collection.push_back(QuadratureType(fe_degree + 1));
  _q_collection.push_back(QuadratureType(fe_degree + 1));

  // Create the heat sources
  boost::property_tree::ptree const &source_database =
      database.get_child("sources");
  // PropertyTreeInput sources.n_beams
  unsigned int const n_beams = source_database.get<unsigned int>("n_beams");
  _heat_sources.resize(n_beams);
  for (unsigned int i = 0; i < n_beams; ++i)
  {
    // PropertyTreeInput sources.beam_X.type
    boost::property_tree::ptree const &beam_database =
        source_database.get_child("beam_" + std::to_string(i));
    std::string type = beam_database.get<std::string>("type");
    if (type == "goldak")
    {
      _heat_sources[i] = std::make_shared<GoldakHeatSource<dim>>(beam_database);
    }
    else if (type == "electron_beam")
    {
      _heat_sources[i] =
          std::make_shared<ElectronBeamHeatSource<dim>>(beam_database);
    }
    else if (type == "cube")
    {
      _heat_sources[i] = std::make_shared<CubeHeatSource<dim>>(beam_database);
    }
    else
    {
      ASSERT_THROW(false, "Error: Beam type '" +
                              beam_database.get<std::string>("type") +
                              "' not recognized.");
    }
  }

  // Create the boundary condition type
  // PropertyTreeInput boundary.type
  std::string boundary_type_str = database.get<std::string>("boundary.type");
  // Parse the string
  size_t pos_str = 0;
  std::string boundary;
  std::string delimiter = ",";
  auto parse_boundary_type = [&](std::string const &boundary)
  {
    if (boundary == "adiabatic")
    {
      _boundary_type = BoundaryType::adiabatic;
    }
    else
    {
      if (boundary == "radiative")
      {
        _boundary_type |= BoundaryType::radiative;
      }
      else if (boundary == "convective")
      {
        _boundary_type |= BoundaryType::convective;
      }
      else
      {
        ASSERT_THROW(false, "Unknown boundary type.");
      }
    }
  };
  while ((pos_str = boundary_type_str.find(delimiter)) != std::string::npos)
  {
    boundary = boundary_type_str.substr(0, pos_str);
    parse_boundary_type(boundary);
    boundary_type_str.erase(0, pos_str + delimiter.length());
  }
  parse_boundary_type(boundary_type_str);

  // Create the thermal operator
  if (std::is_same<MemorySpaceType, dealii::MemorySpace::Host>::value)
    _thermal_operator =
        std::make_shared<ThermalOperator<dim, fe_degree, MemorySpaceType>>(
            communicator, _boundary_type, _material_properties, _heat_sources);
#if defined(ADAMANTINE_HAVE_CUDA) && defined(__CUDACC__)
  else
    _thermal_operator = std::make_shared<
        ThermalOperatorDevice<dim, fe_degree, MemorySpaceType>>(
        communicator, _boundary_type, _material_properties);
#endif

  // Create the time stepping scheme
  boost::property_tree::ptree const &time_stepping_database =
      database.get_child("time_stepping");
  // PropertyTreeInput time_stepping.method
  std::string method = time_stepping_database.get<std::string>("method");
  std::transform(method.begin(), method.end(), method.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (method.compare("forward_euler") == 0)
    _time_stepping =
        std::make_unique<dealii::TimeStepping::ExplicitRungeKutta<LA_Vector>>(
            dealii::TimeStepping::FORWARD_EULER);
  else if (method.compare("rk_third_order") == 0)
    _time_stepping =
        std::make_unique<dealii::TimeStepping::ExplicitRungeKutta<LA_Vector>>(
            dealii::TimeStepping::RK_THIRD_ORDER);
  else if (method.compare("rk_fourth_order") == 0)
    _time_stepping =
        std::make_unique<dealii::TimeStepping::ExplicitRungeKutta<LA_Vector>>(
            dealii::TimeStepping::RK_CLASSIC_FOURTH_ORDER);
  else if (method.compare("heun_euler") == 0)
  {
    _time_stepping = std::make_unique<
        dealii::TimeStepping::EmbeddedExplicitRungeKutta<LA_Vector>>(
        dealii::TimeStepping::HEUN_EULER);
    _embedded_method = true;
  }
  else if (method.compare("bogacki_shampine") == 0)
  {
    _time_stepping = std::make_unique<
        dealii::TimeStepping::EmbeddedExplicitRungeKutta<LA_Vector>>(
        dealii::TimeStepping::BOGACKI_SHAMPINE);
    _embedded_method = true;
  }
  else if (method.compare("dopri") == 0)
  {
    _time_stepping = std::make_unique<
        dealii::TimeStepping::EmbeddedExplicitRungeKutta<LA_Vector>>(
        dealii::TimeStepping::DOPRI);
    _embedded_method = true;
  }
  else if (method.compare("fehlberg") == 0)
  {
    _time_stepping = std::make_unique<
        dealii::TimeStepping::EmbeddedExplicitRungeKutta<LA_Vector>>(
        dealii::TimeStepping::FEHLBERG);
    _embedded_method = true;
  }
  else if (method.compare("cash_karp") == 0)
  {
    _time_stepping = std::make_unique<
        dealii::TimeStepping::EmbeddedExplicitRungeKutta<LA_Vector>>(
        dealii::TimeStepping::CASH_KARP);
    _embedded_method = true;
  }
  else if (method.compare("backward_euler") == 0)
  {
    _time_stepping =
        std::make_unique<dealii::TimeStepping::ImplicitRungeKutta<LA_Vector>>(
            dealii::TimeStepping::BACKWARD_EULER);
    _implicit_method = true;
  }
  else if (method.compare("implicit_midpoint") == 0)
  {
    _time_stepping =
        std::make_unique<dealii::TimeStepping::ImplicitRungeKutta<LA_Vector>>(
            dealii::TimeStepping::IMPLICIT_MIDPOINT);
    _implicit_method = true;
  }
  else if (method.compare("crank_nicolson") == 0)
  {
    _time_stepping =
        std::make_unique<dealii::TimeStepping::ImplicitRungeKutta<LA_Vector>>(
            dealii::TimeStepping::CRANK_NICOLSON);
    _implicit_method = true;
  }
  else if (method.compare("sdirk2") == 0)
  {
    _time_stepping =
        std::make_unique<dealii::TimeStepping::ImplicitRungeKutta<LA_Vector>>(
            dealii::TimeStepping::SDIRK_TWO_STAGES);
    _implicit_method = true;
  }

  if (_embedded_method == true)
  {
    // PropertyTreeInput time_steppping.coarsening_parameter
    double coarsen_param =
        time_stepping_database.get("coarsening_parameter", 1.2);
    // PropertyTreeInput time_steppping.refining_parameter
    double refine_param = time_stepping_database.get("refining_parameter", 0.8);
    // PropertyTreeInput time_stepping.min_time_step
    double min_delta = time_stepping_database.get("min_time_step", 1e-14);
    // PropertyTreeInput time_stepping.max_time_step
    double max_delta = time_stepping_database.get("max_time_step", 1e100);
    // PropertyTreeInput time_stepping.refining_tolerance
    double refine_tol = time_stepping_database.get("refining_tolerance", 1e-8);
    // PropertyTreeInput time_stepping.coarsening_tolerance
    double coarsen_tol =
        time_stepping_database.get("coarsening_tolerance", 1e-12);
    dealii::TimeStepping::EmbeddedExplicitRungeKutta<LA_Vector> *embedded_rk =
        static_cast<
            dealii::TimeStepping::EmbeddedExplicitRungeKutta<LA_Vector> *>(
            _time_stepping.get());
    embedded_rk->set_time_adaptation_parameters(coarsen_param, refine_param,
                                                min_delta, max_delta,
                                                refine_tol, coarsen_tol);
  }

  // If the time stepping scheme is implicit, set the parameters for the solver
  // and create the implicit operator.
  if (_implicit_method == true)
  {
    // PropertyTreeInput time_stepping.max_iteration
    _max_iter = time_stepping_database.get("max_iteration", 1000);
    // PropertyTreeInput time_stepping.tolerance
    _tolerance = time_stepping_database.get("tolerance", 1e-12);
    // PropertyTreeInput time_stepping.right_preconditioning
    _right_preconditioning =
        time_stepping_database.get("right_preconditioning", false);
    // PropertyTreeInput time_stepping.n_tmp_vectors
    _max_n_tmp_vectors = time_stepping_database.get("n_tmp_vectors", 30);
    // PropertyTreeInput time_stepping.newton_max_iteration
    unsigned int newton_max_iter =
        time_stepping_database.get("newton_max_iteration", 100);
    // PropertyTreeInput time_stepping.newton_tolerance
    double newton_tolerance =
        time_stepping_database.get("newton_tolerance", 1e-6);
    dealii::TimeStepping::ImplicitRungeKutta<LA_Vector> *implicit_rk =
        static_cast<dealii::TimeStepping::ImplicitRungeKutta<LA_Vector> *>(
            _time_stepping.get());
    implicit_rk->set_newton_solver_parameters(newton_max_iter,
                                              newton_tolerance);

    // PropertyTreeInput time_stepping.jfnk
    bool jfnk = time_stepping_database.get("jfnk", false);
    _implicit_operator = std::make_unique<ImplicitOperator<MemorySpaceType>>(
        _thermal_operator, jfnk);
  }

  // Set material on part of the domain
  // PropertyTreeInput geometry.material_height
  double const material_height = database.get("geometry.material_height", 1e9);
  for (auto const &cell :
       dealii::filter_iterators(_dof_handler.active_cell_iterators(),
                                dealii::IteratorFilters::LocallyOwnedCell()))
  {
    // If the center of the cell is below material_height, it contains material
    // otherwise it does not.
    if (cell->center()[axis<dim>::z] < material_height)
    {
      cell->set_active_fe_index(0);
      // Set material deposition cos and sin. We arbitrarily choose cos = 1 and
      // sin = 0
      _deposition_cos.push_back(1.);
      _deposition_sin.push_back(0.);
      // Set the initial material as non-melted
      _has_melted.push_back(false);
    }
    else
      cell->set_active_fe_index(1);
  }

  // Set the initial height of the heat source. Right now this is just the
  // maximum heat source height, which can lead to unexpected behavior for
  // different sources with different heights.
  double temp_height = std::numeric_limits<double>::lowest();
  for (auto const &source : _heat_sources)
  {
    temp_height = std::max(temp_height, source->get_current_height(0.0));
  }
  _current_source_height = temp_height;
}

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
void ThermalPhysics<dim, fe_degree, MemorySpaceType,
                    QuadratureType>::setup_dofs()
{
  _dof_handler.distribute_dofs(_fe_collection);
  dealii::IndexSet locally_relevant_dofs;
  dealii::DoFTools::extract_locally_relevant_dofs(_dof_handler,
                                                  locally_relevant_dofs);
  _affine_constraints.clear();
  _affine_constraints.reinit(locally_relevant_dofs);
  dealii::DoFTools::make_hanging_node_constraints(_dof_handler,
                                                  _affine_constraints);
  _affine_constraints.close();

  _thermal_operator->reinit(_dof_handler, _affine_constraints, _q_collection);
}

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
void ThermalPhysics<dim, fe_degree, MemorySpaceType,
                    QuadratureType>::compute_inverse_mass_matrix()
{
  _thermal_operator->compute_inverse_mass_matrix(_dof_handler,
                                                 _affine_constraints);
  if (_implicit_method == true)
    _implicit_operator->set_inverse_mass_matrix(
        _thermal_operator->get_inverse_mass_matrix());
}

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
void ThermalPhysics<dim, fe_degree, MemorySpaceType, QuadratureType>::
    mark_has_melted(
        double const threshold_temperature,
        dealii::LA::distributed::Vector<double, MemorySpaceType> &temperature)
{
  temperature.update_ghost_values();
  auto dofs_per_cell = _dof_handler.get_fe().dofs_per_cell;

  dealii::hp::FEValues<dim> hp_fe_values(
      _dof_handler.get_fe_collection(), _q_collection,
      dealii::UpdateFlags::update_values |
          dealii::UpdateFlags::update_JxW_values);

  unsigned int const n_q_points = _q_collection.max_n_quadrature_points();
  unsigned int cell_id = 0;
  for (auto const &cell : dealii::filter_iterators(
           _dof_handler.active_cell_iterators(),
           dealii::IteratorFilters::LocallyOwnedCell(),
           dealii::IteratorFilters::ActiveFEIndexEqualTo(0)))
  {
    if (!_has_melted[cell_id])
    {
      hp_fe_values.reinit(cell);
      dealii::FEValues<dim> const &fe_values =
          hp_fe_values.get_present_fe_values();

      std::vector<dealii::types::global_dof_index> local_dof_indices(
          fe_values.dofs_per_cell);
      cell->get_dof_indices(local_dof_indices);

      double cell_temperature = 0.0;
      double cell_volume = 0.0;
      for (unsigned int i = 0; i < dofs_per_cell; ++i)
      {
        for (unsigned int q = 0; q < n_q_points; ++q)
        {
          cell_temperature += fe_values.shape_value(i, q) *
                              temperature(local_dof_indices[i]) *
                              fe_values.JxW(q);
          cell_volume += fe_values.shape_value(i, q) * fe_values.JxW(q);
        }
      }
      cell_temperature /= cell_volume;

      // Set the indicator that this cell has melted
      if (cell_temperature > threshold_temperature)
      {
        _has_melted[cell_id] = true;
      }
    }
    ++cell_id;
  }
}

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
void ThermalPhysics<dim, fe_degree, MemorySpaceType, QuadratureType>::
    add_material(
        std::vector<std::vector<
            typename dealii::DoFHandler<dim>::active_cell_iterator>> const
            &elements_to_activate,
        std::vector<double> const &new_deposition_cos,
        std::vector<double> const &new_deposition_sin,
        std::vector<bool> &new_has_melted, unsigned int const activation_start,
        unsigned int const activation_end,
        double const new_material_temperature,
        dealii::LA::distributed::Vector<double, MemorySpaceType> &solution)
{
#ifdef ADAMANTINE_WITH_CALIPER
  CALI_CXX_MARK_FUNCTION;
#endif

  // Update the material state from the ThermalOperator to MaterialProperty
  // because, for now, we need to use state from MaterialProperty to perform the
  // transfer to the refined mesh.
  set_state_to_material_properties();

  _thermal_operator->clear();
  // The data on each cell is stored in the following order: solution, direction
  // of deposition (cosine and sine), prior melting indictor, and state ratio.
  std::vector<std::vector<double>> data_to_transfer;
  unsigned int const n_dofs_per_cell = _dof_handler.get_fe().n_dofs_per_cell();
  unsigned int const direction_data_size = 2;
  unsigned int const phase_history_data_size = 1;
  unsigned int constexpr n_material_states =
      static_cast<unsigned int>(adamantine::MaterialState::SIZE);
  unsigned int const data_size_per_cell =
      n_dofs_per_cell + direction_data_size + phase_history_data_size +
      n_material_states;
  dealii::Vector<double> cell_solution(n_dofs_per_cell);
  std::vector<double> dummy_cell_data(data_size_per_cell,
                                      std::numeric_limits<double>::infinity());

  solution.update_ghost_values();

  adamantine::MemoryBlockView<double, MemorySpaceType> material_state_view =
      _material_properties.get_state();
  adamantine::MemoryBlock<double, dealii::MemorySpace::Host>
      material_state_host(material_state_view.extent(0),
                          material_state_view.extent(1));
  typename decltype(material_state_view)::memory_space memspace;
  adamantine::deep_copy(material_state_host.data(), dealii::MemorySpace::Host{},
                        material_state_view.data(), memspace,
                        material_state_view.size());

  // We need to move the solution on the host because we cannot use
  // CellDataTransfer on the device.
  dealii::IndexSet rw_index_set = solution.locally_owned_elements();
  rw_index_set.add_indices(solution.get_partitioner()->ghost_indices());
  dealii::LA::ReadWriteVector<double> rw_solution(rw_index_set);
  rw_solution.import(solution, dealii::VectorOperation::insert);

  adamantine::MemoryBlockView<double, dealii::MemorySpace::Host>
      state_host_view(material_state_host);
  unsigned int cell_id = 0;
  unsigned int active_cell_id = 0;
  std::map<typename dealii::DoFHandler<dim>::active_cell_iterator, int>
      cell_to_id;
  for (auto const &cell : _dof_handler.active_cell_iterators())
  {
    if (cell->is_locally_owned())
    {
      if (cell->active_fe_index() == 0)
      {
        std::vector<double> cell_data(
            direction_data_size + phase_history_data_size + n_material_states);
        cell->get_dof_values(rw_solution, cell_solution);
        cell_data.insert(cell_data.begin(), cell_solution.begin(),
                         cell_solution.end());
        cell_data[n_dofs_per_cell] = _deposition_cos[cell_id];
        cell_data[n_dofs_per_cell + 1] = _deposition_sin[cell_id];

        if (_has_melted[cell_id])
          cell_data[n_dofs_per_cell + direction_data_size] = 1.0;
        else
          cell_data[n_dofs_per_cell + direction_data_size] = 0.0;

        for (unsigned int i = 0; i < n_material_states; ++i)
          cell_data[n_dofs_per_cell + direction_data_size +
                    phase_history_data_size + i] = state_host_view(i, cell_id);
        data_to_transfer.push_back(cell_data);

        ++cell_id;
      }
      else
      {
        std::vector<double> cell_data = dummy_cell_data;
        for (unsigned int i = 0; i < n_material_states; ++i)
          cell_data[n_dofs_per_cell + direction_data_size +
                    phase_history_data_size + i] = state_host_view(i, cell_id);
        data_to_transfer.push_back(cell_data);
      }
    }
    else
    {
      data_to_transfer.push_back(dummy_cell_data);
    }
    cell_to_id[cell] = active_cell_id;
    ++active_cell_id;
  }

  // Activate elements by updating the fe_index
  for (unsigned int i = activation_start; i < activation_end; ++i)
  {
    for (auto const &cell : elements_to_activate[i])
    {
      if (cell->active_fe_index() != 0)
      {
        cell->set_future_fe_index(0);
        data_to_transfer[cell_to_id[cell]][n_dofs_per_cell] =
            new_deposition_cos[i];
        data_to_transfer[cell_to_id[cell]][n_dofs_per_cell + 1] =
            new_deposition_sin[i];

        if (data_to_transfer[cell_to_id[cell]]
                            [n_dofs_per_cell + direction_data_size] > 0.5)
          new_has_melted[i] = true;
        else
          new_has_melted[i] = false;
      }
    }
  }

  dealii::parallel::distributed::Triangulation<dim> &triangulation =
      dynamic_cast<dealii::parallel::distributed::Triangulation<dim> &>(
          const_cast<dealii::Triangulation<dim> &>(
              _dof_handler.get_triangulation()));
  triangulation.prepare_coarsening_and_refinement();
  dealii::parallel::distributed::CellDataTransfer<
      dim, dim, std::vector<std::vector<double>>>
      cell_data_trans(triangulation);
  cell_data_trans.prepare_for_coarsening_and_refinement(data_to_transfer);

#ifdef ADAMANTINE_WITH_CALIPER
  CALI_MARK_BEGIN("refine triangulation");
#endif
  triangulation.execute_coarsening_and_refinement();
#ifdef ADAMANTINE_WITH_CALIPER
  CALI_MARK_END("refine triangulation");
#endif

  setup_dofs();

  // Update MaterialProperty DoFHandler and resize the state vectors
  _material_properties.reinit_dofs();

  // Recompute the inverse of the mass matrix
  compute_inverse_mass_matrix();

  initialize_dof_vector(std::numeric_limits<double>::infinity(), solution);
  rw_index_set = solution.locally_owned_elements();
  rw_index_set.add_indices(solution.get_partitioner()->ghost_indices());
  rw_solution.reinit(rw_index_set);
  for (auto val : solution.locally_owned_elements())
    rw_solution[val] = new_material_temperature;

  // Unpack the material state and repopulate the material state
  std::vector<std::vector<double>> transferred_data(
      triangulation.n_active_cells(), std::vector<double>(data_size_per_cell));
  cell_data_trans.unpack(transferred_data);
  material_state_view = _material_properties.get_state();
  material_state_host.reinit(material_state_view.extent(0),
                             material_state_view.extent(1));
  state_host_view.reinit(material_state_host);
  _deposition_cos.clear();
  _deposition_sin.clear();
  _has_melted.clear();
  cell_id = 0;
  active_cell_id = 0;
  for (auto const &cell : _dof_handler.active_cell_iterators())
  {
    if (cell->is_locally_owned())
    {
      if (transferred_data[active_cell_id][0] !=
          std::numeric_limits<double>::infinity())
      {
        std::copy(transferred_data[active_cell_id].begin(),
                  transferred_data[active_cell_id].begin() + n_dofs_per_cell,
                  cell_solution.begin());
        cell->set_dof_values(cell_solution, rw_solution);
      }

      if (cell->active_fe_index() == 0)
      {
        _deposition_cos.push_back(
            transferred_data[active_cell_id][n_dofs_per_cell]);
        _deposition_sin.push_back(
            transferred_data[active_cell_id][n_dofs_per_cell + 1]);
        if (transferred_data[active_cell_id]
                            [n_dofs_per_cell + direction_data_size] > 0.5)
          _has_melted.push_back(true);
        else
          _has_melted.push_back(false);
      }
      for (unsigned int i = 0; i < n_material_states; ++i)
      {
        state_host_view(i, cell_id) =
            transferred_data[active_cell_id]
                            [n_dofs_per_cell + direction_data_size +
                             phase_history_data_size + i];
      }
      ++cell_id;
    }
    ++active_cell_id;
  }
  deep_copy(material_state_view, state_host_view);
  get_state_from_material_properties();
  _thermal_operator->set_material_deposition_orientation(_deposition_cos,
                                                         _deposition_sin);

  // Communicate the results.
  solution.import(rw_solution, dealii::VectorOperation::insert);

  // Set the value to the newly create DoFs. Here we need to be careful with the
  // hanging nodes. When there is a hanging node, the dofs at the vertices are
  // "doubled": there is a dof associated to the coarse cell and a dof
  // associated to the fine cell. The final value is decided by
  // AffineConstraints. Thus, we need to make sure that the newly activated
  // cells are at the same level than their neighbors.
  rw_solution.reinit(solution.locally_owned_elements());
  rw_solution.import(solution, dealii::VectorOperation::insert);
  std::for_each(rw_solution.begin(), rw_solution.end(),
                [&](double &val)
                {
                  if (val == std::numeric_limits<double>::infinity())
                  {
                    val = new_material_temperature;
                  }
                });
  solution.import(rw_solution, dealii::VectorOperation::insert);
}

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
void ThermalPhysics<dim, fe_degree, MemorySpaceType, QuadratureType>::
    update_physics_parameters(
        boost::property_tree::ptree const &heat_source_database)
{
  // Update the heat source from heat_source_database to reflect changes during
  // the simulation (i.e. due to data assimilation)
  unsigned int source_index = 0;
  for (auto const &source : _heat_sources)
  {
    // PropertyTreeInput sources.beam_X
    boost::property_tree::ptree const &beam_database =
        heat_source_database.get_child("beam_" + std::to_string(source_index));

    // PropertyTreeInput sources.beam_X.type
    std::string type = beam_database.get<std::string>("type");

    if (type == "goldak" || type == "electron_beam")
      source->set_beam_properties(beam_database);

    source_index++;
  }
}

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
double ThermalPhysics<dim, fe_degree, MemorySpaceType, QuadratureType>::
    evolve_one_time_step(
        double t, double delta_t,
        dealii::LA::distributed::Vector<double, MemorySpaceType> &solution,
        std::vector<Timer> &timers)
{
  // Update the height of the heat source. Right now this is just the
  // maximum heat source height, which can lead to unexpected behavior for
  // different sources with different heights.
  double temp_height = std::numeric_limits<double>::lowest();
  for (auto const &source : _heat_sources)
  {
    temp_height = std::max(temp_height, source->get_current_height(t));
  }
  _current_source_height = temp_height;

  auto eval = [&](double const t, LA_Vector const &y)
  { return evaluate_thermal_physics(t, y, timers); };
  auto id_m_Jinv = [&](double const t, double const tau, LA_Vector const &y)
  { return id_minus_tau_J_inverse(t, tau, y, timers); };

  double time = _time_stepping->evolve_one_time_step(eval, id_m_Jinv, t,
                                                     delta_t, solution);

  // If the method is embedded, get the next time step. Otherwise, just use the
  // current time step.
  if (_embedded_method == false)
    _delta_t_guess = delta_t;
  else
  {
    dealii::TimeStepping::EmbeddedExplicitRungeKutta<LA_Vector> *embedded_rk =
        static_cast<
            dealii::TimeStepping::EmbeddedExplicitRungeKutta<LA_Vector> *>(
            _time_stepping.get());
    _delta_t_guess = embedded_rk->get_status().delta_t_guess;
  }

  // Return the time at the end of the time step. This may be different than
  // t+delta_t for embedded methods.
  return time;
}

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
void ThermalPhysics<dim, fe_degree, MemorySpaceType, QuadratureType>::
    initialize_dof_vector(
        dealii::LA::distributed::Vector<double, MemorySpaceType> &vector) const
{
  _thermal_operator->initialize_dof_vector(vector);
}

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
void ThermalPhysics<dim, fe_degree, MemorySpaceType, QuadratureType>::
    initialize_dof_vector(
        double const value,
        dealii::LA::distributed::Vector<double, MemorySpaceType> &vector) const
{
  // Resize the vector
  _thermal_operator->initialize_dof_vector(vector);

  init_dof_vector<dim, fe_degree, MemorySpaceType>(value, vector);
}

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
void ThermalPhysics<dim, fe_degree, MemorySpaceType,
                    QuadratureType>::get_state_from_material_properties()
{
  _thermal_operator->get_state_from_material_properties();
}

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
void ThermalPhysics<dim, fe_degree, MemorySpaceType,
                    QuadratureType>::set_state_to_material_properties()
{
  _thermal_operator->set_state_to_material_properties();
}

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
dealii::LA::distributed::Vector<double, MemorySpaceType>
ThermalPhysics<dim, fe_degree, MemorySpaceType, QuadratureType>::
    evaluate_thermal_physics(
        double const t,
        dealii::LA::distributed::Vector<double, MemorySpaceType> const &y,
        std::vector<Timer> &timers) const
{
#ifdef ADAMANTINE_WITH_CALIPER
  CALI_CXX_MARK_FUNCTION;
#endif
  if constexpr (std::is_same<MemorySpaceType, dealii::MemorySpace::Host>::value)
  {
    return evaluate_thermal_physics_impl<dim, fe_degree, MemorySpaceType>(
        _thermal_operator, t, _current_source_height, y, timers);
  }
  else
  {
    return evaluate_thermal_physics_impl<dim, fe_degree, MemorySpaceType>(
        _thermal_operator, _fe_collection, t, _dof_handler, _heat_sources,
        _current_source_height, _boundary_type, _material_properties,
        _affine_constraints, y, timers);
  }

  // Dummy to silence warning
  return dealii::LA::distributed::Vector<double, MemorySpaceType>();
}

template <int dim, int fe_degree, typename MemorySpaceType,
          typename QuadratureType>
dealii::LA::distributed::Vector<double, MemorySpaceType>
ThermalPhysics<dim, fe_degree, MemorySpaceType, QuadratureType>::
    id_minus_tau_J_inverse(
        double const /*t*/, double const tau,
        dealii::LA::distributed::Vector<double, MemorySpaceType> const &y,
        std::vector<Timer> &timers) const
{
  timers[evol_time_J_inv].start();
  _implicit_operator->set_tau(tau);
  dealii::LA::distributed::Vector<double, MemorySpaceType> solution(
      y.get_partitioner());

  // TODO Add a geometric multigrid preconditioner.
  dealii::PreconditionIdentity preconditioner;

  dealii::SolverControl solver_control(_max_iter, _tolerance * y.l2_norm());
  // We need to inverse (I - tau M^{-1} J). While M^{-1} and J are SPD,
  // (I - tau M^{-1} J) is symmetric indefinite in the general case.
  typename dealii::SolverGMRES<
      dealii::LA::distributed::Vector<double, MemorySpaceType>>::AdditionalData
      additional_data(_max_n_tmp_vectors, _right_preconditioning);
  dealii::SolverGMRES<dealii::LA::distributed::Vector<double, MemorySpaceType>>
      solver(solver_control, additional_data);
  solver.solve(*_implicit_operator, solution, y, preconditioner);

  timers[evol_time_J_inv].stop();

  return solution;
}
} // namespace adamantine

#endif
