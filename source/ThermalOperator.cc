/* Copyright (c) 2016 - 2022, the adamantine authors.
 *
 * This file is subject to the Modified BSD License and may not be distributed
 * without copyright and license information. Please refer to the file LICENSE
 * for the text and further information on this license.
 */

#include <ThermalOperator.hh>
#include <instantiation.hh>
#include <utils.hh>

#include <deal.II/base/aligned_vector.h>
#include <deal.II/base/index_set.h>
#include <deal.II/base/types.h>
#include <deal.II/base/vectorization.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/mapping_q1.h>
#include <deal.II/grid/filtered_iterator.h>
#include <deal.II/hp/fe_values.h>
#include <deal.II/matrix_free/fe_evaluation.h>

namespace adamantine
{

template <int dim, int fe_degree, typename MemorySpaceType>
ThermalOperator<dim, fe_degree, MemorySpaceType>::ThermalOperator(
    MPI_Comm const &communicator, BoundaryType boundary_type,
    MaterialProperty<dim, MemorySpaceType> &material_properties,
    std::vector<std::shared_ptr<HeatSource<dim>>> const &heat_sources)
    : _communicator(communicator), _boundary_type(boundary_type),
      _material_properties(material_properties), _heat_sources(heat_sources),
      _inverse_mass_matrix(
          new dealii::LA::distributed::Vector<double, MemorySpaceType>())
{
  _matrix_free_data.tasks_parallel_scheme =
      dealii::MatrixFree<dim, double>::AdditionalData::partition_color;
  _matrix_free_data.mapping_update_flags =
      dealii::update_values | dealii::update_gradients |
      dealii::update_JxW_values | dealii::update_quadrature_points;
  _matrix_free_data.mapping_update_flags_inner_faces =
      dealii::update_values | dealii::update_JxW_values;
  _matrix_free_data.mapping_update_flags_boundary_faces =
      dealii::update_values | dealii::update_JxW_values;
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::reinit(
    dealii::DoFHandler<dim> const &dof_handler,
    dealii::AffineConstraints<double> const &affine_constraints,
    dealii::hp::QCollection<1> const &q_collection)
{
  _matrix_free.reinit(dealii::StaticMappingQ1<dim>::mapping, dof_handler,
                      affine_constraints, q_collection, _matrix_free_data);
  _affine_constraints = &affine_constraints;

  // Compute mapping between DoFHandler cells and the MatrixFree cells
  _cell_it_to_mf_cell_map.clear();
  unsigned int const n_cells = _matrix_free.n_cell_batches();
  for (unsigned int cell = 0; cell < n_cells; ++cell)
    for (unsigned int i = 0;
         i < _matrix_free.n_active_entries_per_cell_batch(cell); ++i)
    {
      typename dealii::DoFHandler<dim>::cell_iterator cell_it =
          _matrix_free.get_cell_iterator(cell, i);
      _cell_it_to_mf_cell_map[cell_it] = std::make_pair(cell, i);
    }
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::cell_local_mass(
    dealii::MatrixFree<dim, double> const &data,
    dealii::LA::distributed::Vector<double, MemorySpaceType> &dst,
    dealii::LA::distributed::Vector<double, MemorySpaceType> const &src,
    std::pair<unsigned int, unsigned int> const &cell_range) const
{
  // Get the subrange of cells associated with the fe index 0
  std::pair<unsigned int, unsigned int> cell_subrange =
      data.create_cell_subrange_hp_by_index(cell_range, 0);
  dealii::FEEvaluation<dim, fe_degree, fe_degree + 1, 1, double> fe_eval(data);

  // Loop over the "cells". Note that we don't really work on a cell but on a
  // set of quadrature point.
  for (unsigned int cell = cell_subrange.first; cell < cell_subrange.second;
       ++cell)
  {
    // Reinit fe_eval on the current cell
    fe_eval.reinit(cell);
    // Store in a local vector the local values of src
    fe_eval.read_dof_values(src);
    // Evaluate the shape function on the reference cell
    fe_eval.evaluate(dealii::EvaluationFlags::values);
    // Apply the Jacobian of the transformation
    for (unsigned int q = 0; q < fe_eval.n_q_points; ++q)
    {
      fe_eval.submit_value(fe_eval.get_value(q), q);
    }
    // Sum over the quadrature points.
    fe_eval.integrate(dealii::EvaluationFlags::values);
    fe_eval.distribute_local_to_global(dst);
  }
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::
    compute_inverse_mass_matrix(
        dealii::DoFHandler<dim> const &dof_handler,
        dealii::AffineConstraints<double> const &affine_constraints)
{
  // Compute the inverse of the mass matrix
  dealii::hp::QCollection<dim> mass_q_collection;
  mass_q_collection.push_back(dealii::QGaussLobatto<dim>(fe_degree + 1));
  mass_q_collection.push_back(dealii::QGaussLobatto<dim>(2));

  typename dealii::MatrixFree<dim, double>::AdditionalData
      mass_matrix_free_data;
  mass_matrix_free_data.tasks_parallel_scheme =
      dealii::MatrixFree<dim, double>::AdditionalData::partition_color;
  mass_matrix_free_data.mapping_update_flags =
      dealii::update_values | dealii::update_JxW_values;

  dealii::MatrixFree<dim, double> mass_matrix_free;
  mass_matrix_free.reinit(dealii::StaticMappingQ1<dim>::mapping, dof_handler,
                          affine_constraints, mass_q_collection,
                          mass_matrix_free_data);
  mass_matrix_free.initialize_dof_vector(*_inverse_mass_matrix);
  dealii::LA::distributed::Vector<double, MemorySpaceType> unit_vector;
  mass_matrix_free.initialize_dof_vector(unit_vector);
  unit_vector = 1.;
  mass_matrix_free.cell_loop(&ThermalOperator::cell_local_mass, this,
                             *_inverse_mass_matrix, unit_vector);
  // Because cell_loop resolves the constraints, the constrained dofs are not
  // called they stay at zero. Thus, we need to force the value on the
  // constrained dofs by hand.
  std::vector<unsigned int> const &constrained_dofs =
      mass_matrix_free.get_constrained_dofs();
  for (auto &dof : constrained_dofs)
    _inverse_mass_matrix->local_element(dof) += 1.;

  _inverse_mass_matrix->compress(dealii::VectorOperation::add);

  unsigned int const local_size = _inverse_mass_matrix->locally_owned_size();
  for (unsigned int k = 0; k < local_size; ++k)
  {
    _inverse_mass_matrix->local_element(k) =
        1. / _inverse_mass_matrix->local_element(k);
  }
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::clear()
{
  _cell_it_to_mf_cell_map.clear();
  _matrix_free.clear();
  _inverse_mass_matrix->reinit(0);
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::vmult(
    dealii::LA::distributed::Vector<double, MemorySpaceType> &dst,
    dealii::LA::distributed::Vector<double, MemorySpaceType> const &src) const
{
  dst = 0.;
  vmult_add(dst, src);
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::Tvmult(
    dealii::LA::distributed::Vector<double, MemorySpaceType> &dst,
    dealii::LA::distributed::Vector<double, MemorySpaceType> const &src) const
{
  dst = 0.;
  Tvmult_add(dst, src);
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::vmult_add(
    dealii::LA::distributed::Vector<double, MemorySpaceType> &dst,
    dealii::LA::distributed::Vector<double, MemorySpaceType> const &src) const
{
  // Execute the matrix-free matrix-vector multiplication

  // If we use adiabatic boundary condition, we have nothing to do on the faces
  // of the cell
  if (_boundary_type & BoundaryType::adiabatic)
  {
    _matrix_free.cell_loop(&ThermalOperator::cell_local_apply, this, dst, src);
  }
  else
  {
    // MatrixFree::loop works like cell_loop but also allow computation on
    // internal faces and boundary faces. Here, we use the same function for
    // both cases and apply the face condition only at the boundary of the
    // activated domain.
    _matrix_free.loop(&ThermalOperator::cell_local_apply,
                      &ThermalOperator::face_local_apply,
                      &ThermalOperator::face_local_apply, this, dst, src);
  }

  // Because cell_loop resolves the constraints, the constrained dofs are not
  // called they stay at zero. Thus, we need to force the value on the
  // constrained dofs by hand. The variable scaling is used so that we get the
  // right order of magnitude.
  // TODO: for now the value of scaling is set to 1
  double const scaling = 1.;
  std::vector<unsigned int> const &constrained_dofs =
      _matrix_free.get_constrained_dofs();
  for (auto &dof : constrained_dofs)
    dst.local_element(dof) += scaling * src.local_element(dof);
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::Tvmult_add(
    dealii::LA::distributed::Vector<double, MemorySpaceType> &dst,
    dealii::LA::distributed::Vector<double, MemorySpaceType> const &src) const
{
  // The system of equation is symmetric so we can use vmult_add
  vmult_add(dst, src);
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::update_state_ratios(
    unsigned int cell, unsigned int q,
    dealii::VectorizedArray<double> temperature,
    std::array<dealii::VectorizedArray<double>, g_n_material_states>
        &state_ratios) const
{
  unsigned int constexpr liquid =
      static_cast<unsigned int>(MaterialState::liquid);
  unsigned int constexpr powder =
      static_cast<unsigned int>(MaterialState::powder);
  unsigned int constexpr solid =
      static_cast<unsigned int>(MaterialState::solid);

  // Loop over the vectorized arrays
  for (unsigned int n = 0; n < temperature.size(); ++n)
  {
    // Get the material id at this point
    dealii::types::material_id const material_id = _material_id(cell, q)[n];

    // Get the material thermodynamic properties
    double const solidus =
        _material_properties.get(material_id, Property::solidus);
    double const liquidus =
        _material_properties.get(material_id, Property::liquidus);

    // Update the state ratios
    state_ratios[powder] = _powder_ratio(cell, q);

    if (temperature[n] < solidus)
      state_ratios[liquid][n] = 0.;
    else if (temperature[n] > liquidus)
      state_ratios[liquid][n] = 1.;
    else
    {
      state_ratios[liquid][n] =
          (temperature[n] - solidus) / (liquidus - solidus);
    }
    // Because the powder can only become liquid, the solid can only
    // become liquid, and the liquid can only become solid, the ratio of
    // powder can only decrease.
    state_ratios[powder][n] =
        std::min(1. - state_ratios[liquid][n], state_ratios[powder][n]);
    // Use max to make sure that we don't create matter because of
    // round-off.
    state_ratios[solid][n] =
        std::max(1. - state_ratios[liquid][n] - state_ratios[powder][n], 0.);
  }

  _liquid_ratio(cell, q) = state_ratios[liquid];
  _powder_ratio(cell, q) = state_ratios[powder];
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::update_face_state_ratios(
    unsigned int face, unsigned int q,
    dealii::VectorizedArray<double> temperature,
    std::array<dealii::VectorizedArray<double>, g_n_material_states>
        &face_state_ratios) const
{
  unsigned int constexpr liquid =
      static_cast<unsigned int>(MaterialState::liquid);
  unsigned int constexpr powder =
      static_cast<unsigned int>(MaterialState::powder);
  unsigned int constexpr solid =
      static_cast<unsigned int>(MaterialState::solid);

  // Loop over the vectorized arrays
  for (unsigned int n = 0; n < temperature.size(); ++n)
  {
    // Get the material id at this point
    dealii::types::material_id const material_id =
        _face_material_id(face, q)[n];

    // Get the material thermodynamic properties
    double const solidus =
        _material_properties.get(material_id, Property::solidus);
    double const liquidus =
        _material_properties.get(material_id, Property::liquidus);

    // Update the state ratios
    face_state_ratios[powder] = _face_powder_ratio(face, q);

    if (temperature[n] < solidus)
      face_state_ratios[liquid][n] = 0.;
    else if (temperature[n] > liquidus)
      face_state_ratios[liquid][n] = 1.;
    else
    {
      face_state_ratios[liquid][n] =
          (temperature[n] - solidus) / (liquidus - solidus);
    }
    // Because the powder can only become liquid, the solid can only
    // become liquid, and the liquid can only become solid, the ratio of
    // powder can only decrease.
    face_state_ratios[powder][n] = std::min(1. - face_state_ratios[liquid][n],
                                            face_state_ratios[powder][n]);
    // Use max to make sure that we don't create matter because of
    // round-off.
    face_state_ratios[solid][n] = std::max(
        1. - face_state_ratios[liquid][n] - face_state_ratios[powder][n], 0.);
  }

  _face_powder_ratio(face, q) = face_state_ratios[powder];
}

template <int dim, int fe_degree, typename MemorySpaceType>
dealii::VectorizedArray<double>
ThermalOperator<dim, fe_degree, MemorySpaceType>::get_inv_rho_cp(
    std::array<dealii::types::material_id,
               dealii::VectorizedArray<double>::size()> const &material_id,
    std::array<dealii::VectorizedArray<double>, g_n_material_states> const
        &state_ratios,
    dealii::VectorizedArray<double> const &temperature,
    dealii::AlignedVector<dealii::VectorizedArray<double>> const
        &temperature_powers) const
{
  // Here we need the specific heat (including the latent heat contribution)
  // and the density

  // First, get the state-independent material properties
  dealii::VectorizedArray<double> solidus, liquidus, latent_heat;
  for (unsigned int n = 0; n < solidus.size(); ++n)
  {
    solidus[n] = _material_properties.get(material_id[n], Property::solidus);
    liquidus[n] = _material_properties.get(material_id[n], Property::liquidus);
    latent_heat[n] =
        _material_properties.get(material_id[n], Property::latent_heat);
  }

  // Now compute the state-dependent properties
  dealii::VectorizedArray<double> density =
      _material_properties.compute_material_property(
          StateProperty::density, material_id.data(), state_ratios.data(),
          temperature, temperature_powers);

  dealii::VectorizedArray<double> specific_heat =
      _material_properties.compute_material_property(
          StateProperty::specific_heat, material_id.data(), state_ratios.data(),
          temperature, temperature_powers);

  // Add in the latent heat contribution
  unsigned int constexpr liquid =
      static_cast<unsigned int>(MaterialState::liquid);

  for (unsigned int n = 0; n < specific_heat.size(); ++n)
  {
    if (state_ratios[liquid][n] > 0.0 && (state_ratios[liquid][n] < 1.0))
    {
      specific_heat[n] += latent_heat[n] / (liquidus[n] - solidus[n]);
    }
  }

  return 1.0 / (density * specific_heat);
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::cell_local_apply(
    dealii::MatrixFree<dim, double> const &data,
    dealii::LA::distributed::Vector<double, MemorySpaceType> &dst,
    dealii::LA::distributed::Vector<double, MemorySpaceType> const &src,
    std::pair<unsigned int, unsigned int> const &cell_range) const
{
  // Get the subrange of cells associated with the fe index 0
  std::pair<unsigned int, unsigned int> cell_subrange =
      data.create_cell_subrange_hp_by_index(cell_range, 0);

  dealii::FEEvaluation<dim, fe_degree, fe_degree + 1, 1, double> fe_eval(data);
  std::array<dealii::VectorizedArray<double>, g_n_material_states>
      state_ratios = {{dealii::make_vectorized_array(-1.0),
                       dealii::make_vectorized_array(-1.0),
                       dealii::make_vectorized_array(-1.0)}};

  // We need powers of temperature to compute the material properties. We
  // could compute it in MaterialProperty but because it's in a hot loop.
  // It's really worth to compute it once and pass it when we compute a
  // material property.
  dealii::AlignedVector<dealii::VectorizedArray<double>> temperature_powers(
      _material_properties.polynomial_order + 1);

  // Loop over the "cells". Note that we don't really work on a cell but on a
  // set of quadrature point.
  for (unsigned int cell = cell_subrange.first; cell < cell_subrange.second;
       ++cell)
  {
    // Reinit fe_eval on the current cell
    fe_eval.reinit(cell);
    // Store in a local vector the local values of src
    fe_eval.read_dof_values(src);
    // Evaluate the function and its gradient on the reference cell
    fe_eval.evaluate(dealii::EvaluationFlags::values |
                     dealii::EvaluationFlags::gradients);
    // Apply the Jacobian of the transformation, multiply by the variable
    // coefficients and the quadrature points
    for (unsigned int q = 0; q < fe_eval.n_q_points; ++q)
    {
      auto temperature = fe_eval.get_value(q);
      // Precompute the powers of temperature.
      for (unsigned int i = 0; i <= _material_properties.polynomial_order; ++i)
      {
        // FIXME Need to cast i to double due to a limitation in deal.II 9.5
        temperature_powers[i] = std::pow(temperature, static_cast<double>(i));
      }

      // Calculate the local material properties
      update_state_ratios(cell, q, temperature, state_ratios);
      auto material_id = _material_id(cell, q);
      auto inv_rho_cp = get_inv_rho_cp(material_id, state_ratios, temperature,
                                       temperature_powers);
      auto th_conductivity_grad = fe_eval.get_gradient(q);

      // In 2D we only use x and z, and there are no deposition angle
      if constexpr (dim == 2)
      {
        th_conductivity_grad[axis<dim>::x] *=
            _material_properties.compute_material_property(
                StateProperty::thermal_conductivity_x, material_id.data(),
                state_ratios.data(), temperature, temperature_powers);
        th_conductivity_grad[axis<dim>::z] *=
            _material_properties.compute_material_property(
                StateProperty::thermal_conductivity_z, material_id.data(),
                state_ratios.data(), temperature, temperature_powers);
      }

      if constexpr (dim == 3)
      {
        auto const th_conductivity_grad_x = th_conductivity_grad[axis<dim>::x];
        auto const th_conductivity_grad_y = th_conductivity_grad[axis<dim>::y];
        auto const thermal_conductivity_x =
            _material_properties.compute_material_property(
                StateProperty::thermal_conductivity_x, material_id.data(),
                state_ratios.data(), temperature, temperature_powers);
        auto const thermal_conductivity_y =
            _material_properties.compute_material_property(
                StateProperty::thermal_conductivity_y, material_id.data(),
                state_ratios.data(), temperature, temperature_powers);

        auto cos = _deposition_cos(cell, q);
        auto sin = _deposition_sin(cell, q);

        // The rotation is performed using the following formula
        //
        // (cos  -sin) (x  0) ( cos  sin)
        // (sin   cos) (0  y) (-sin  cos)
        // =
        // ((x*cos^2 + y*sin^2)  ((x-y) * (sin*cos)))
        // (((x-y) * (sin*cos))  (x*sin^2 + y*cos^2))

        th_conductivity_grad[axis<dim>::x] =
            (thermal_conductivity_x * cos * cos +
             thermal_conductivity_y * sin * sin) *
                th_conductivity_grad_x +
            ((thermal_conductivity_x - thermal_conductivity_y) * sin * cos) *
                th_conductivity_grad_y;
        th_conductivity_grad[axis<dim>::y] =
            ((thermal_conductivity_x - thermal_conductivity_y) * sin * cos) *
                th_conductivity_grad_x +
            (thermal_conductivity_x * sin * sin +
             thermal_conductivity_y * cos * cos) *
                th_conductivity_grad_y;

        // There is no deposition angle for the z axis
        th_conductivity_grad[axis<dim>::z] *=
            _material_properties.compute_material_property(
                StateProperty::thermal_conductivity_z, material_id.data(),
                state_ratios.data(), temperature, temperature_powers);
      }

      fe_eval.submit_gradient(-inv_rho_cp * th_conductivity_grad, q);

      // Compute source term
      dealii::Point<dim, dealii::VectorizedArray<double>> const &q_point =
          fe_eval.quadrature_point(q);

      dealii::VectorizedArray<double> quad_pt_source = 0.0;
      for (unsigned int i = 0;
           i < _matrix_free.n_active_entries_per_cell_batch(cell); ++i)
      {
        dealii::Point<dim> q_point_loc;
        for (unsigned int d = 0; d < dim; ++d)
          q_point_loc(d) = q_point(d)[i];

        for (auto &beam : _heat_sources)
          quad_pt_source[i] += beam->value(q_point_loc, _current_source_height);
      }
      quad_pt_source *= inv_rho_cp;

      fe_eval.submit_value(quad_pt_source, q);
    }
    // Sum over the quadrature points.
    fe_eval.integrate(dealii::EvaluationFlags::values |
                      dealii::EvaluationFlags::gradients);
    fe_eval.distribute_local_to_global(dst);
  }
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::face_local_apply(
    dealii::MatrixFree<dim, double> const &data,
    dealii::LA::distributed::Vector<double, MemorySpaceType> &dst,
    dealii::LA::distributed::Vector<double, MemorySpaceType> const &src,
    std::pair<unsigned int, unsigned int> const &face_range) const
{
  // Get the fe_indices of the cells that share faces in face_range;
  auto const adjacent_cells_fe_index = data.get_face_range_category(face_range);
  // We now have four cases:
  //  - cell_1 = cell_2 = FE_Q: internal face of the activated domain
  //  - cell_1/2 = FE_Q and cell_2/1 = FE_Nothing/does not exit: boundary of the
  //      activated domain
  //  - cell_1/2 = FE_Nothing and cell_2/1 = does not exit: external boundary of
  //      the deactivated domain
  //  - cell_1 = cell_2 = FE_Nothing: internal face of the non-activated domain
  // Since we only care on the faces that are at the boundary of the activated
  // domain, we need to check that cell_1 is different than cell_2 and that at
  // one of the two cells is using FE_Q
  if (adjacent_cells_fe_index.first == adjacent_cells_fe_index.second)
  {
    return;
  }
  if ((adjacent_cells_fe_index.first != 0 &&
       adjacent_cells_fe_index.second != 0))
  {
    return;
  }

  // Create the FEFaceEvaluation object. The boolean in the constructor is used
  // to decided which cell the face should be exterior to.
  dealii::FEFaceEvaluation<dim, fe_degree, fe_degree + 1, 1, double>
      fe_face_eval(data, adjacent_cells_fe_index.first == 0);
  std::array<dealii::VectorizedArray<double>, g_n_material_states>
      face_state_ratios = {{dealii::make_vectorized_array(-1.0),
                            dealii::make_vectorized_array(-1.0),
                            dealii::make_vectorized_array(-1.0)}};
  // Create variables used to compute boundary conditions.
  auto conv_temperature_infty = dealii::make_vectorized_array<double>(0.);
  auto conv_heat_transfer_coef = dealii::make_vectorized_array<double>(0.);
  auto rad_temperature_infty = dealii::make_vectorized_array<double>(0.);
  auto rad_heat_transfer_coef = dealii::make_vectorized_array<double>(0.);

  // We need powers of temperature to compute the material properties. We
  // could compute it in MaterialProperty but because it's in a hot loop.
  // It's really worth to compute it once and pass it when we compute a
  // material property.
  dealii::AlignedVector<dealii::VectorizedArray<double>> temperature_powers(
      _material_properties.polynomial_order + 1);

  // Loop over the faces
  for (unsigned int face = face_range.first; face < face_range.second; ++face)
  {
    // Reinit fe_face_eval on the current face
    fe_face_eval.reinit(face);
    // Store in a local vector the local values of src
    fe_face_eval.read_dof_values(src);
    // Evalue the function on the reference cell
    fe_face_eval.evaluate(dealii::EvaluationFlags::values);
    // Apply the Jacobian of the transformation, mutliply by the variable
    // coefficients and the quadrature points
    for (unsigned int q = 0; q < fe_face_eval.n_q_points; ++q)
    {
      auto temperature = fe_face_eval.get_value(q);
      // Precompute the powers of temperature.
      for (unsigned int i = 0; i <= _material_properties.polynomial_order; ++i)
      {
        // FIXME Need to cast i to double due to a limitation in deal.II 9.5
        temperature_powers[i] = std::pow(temperature, static_cast<double>(i));
      }

      // Compute the local_properties
      auto material_id = _face_material_id(face, q);
      update_face_state_ratios(face, q, temperature, face_state_ratios);
      auto const inv_rho_cp = get_inv_rho_cp(material_id, face_state_ratios,
                                             temperature, temperature_powers);
      if (_boundary_type & BoundaryType::convective)
      {
        for (unsigned int n = 0; n < conv_temperature_infty.size(); ++n)
        {
          conv_temperature_infty[n] = _material_properties.get(
              material_id[n], Property::convection_temperature_infty);
        }
        conv_heat_transfer_coef =
            _material_properties.compute_material_property(
                StateProperty::convection_heat_transfer_coef,
                material_id.data(), face_state_ratios.data(), temperature,
                temperature_powers);
      }
      if (_boundary_type & BoundaryType::radiative)
      {
        for (unsigned int n = 0; n < rad_temperature_infty.size(); ++n)
        {
          rad_temperature_infty[n] = _material_properties.get(
              material_id[n], Property::radiation_temperature_infty);
        }

        // We need the radiation heat transfer coefficient but it is not a real
        // material property but it is derived from other material
        // properties: h_rad = emissitivity * stefan-boltzmann constant * (T
        // + T_infty) (T^2 + T^2_infty).
        rad_heat_transfer_coef =
            _material_properties.compute_material_property(
                StateProperty::emissivity, material_id.data(),
                face_state_ratios.data(), temperature, temperature_powers) *
            Constant::stefan_boltzmann * (temperature + rad_temperature_infty) *
            (temperature * temperature +
             rad_temperature_infty * rad_temperature_infty);
      }

      auto const boundary_val =
          -inv_rho_cp *
          (conv_heat_transfer_coef * (temperature - conv_temperature_infty) +
           rad_heat_transfer_coef * (temperature - rad_temperature_infty));
      fe_face_eval.submit_value(boundary_val * fe_face_eval.get_value(q), q);
    }
    // Sum over the quadrature points
    fe_face_eval.integrate(dealii::EvaluationFlags::values);
    fe_face_eval.distribute_local_to_global(dst);
  }
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree,
                     MemorySpaceType>::get_state_from_material_properties()
{
  unsigned int const n_cells = _matrix_free.n_cell_batches();
  dealii::FEEvaluation<dim, fe_degree, fe_degree + 1, 1, double> fe_eval(
      _matrix_free);

  _liquid_ratio.reinit(n_cells, fe_eval.n_q_points);
  _powder_ratio.reinit(n_cells, fe_eval.n_q_points);
  _material_id.reinit(n_cells, fe_eval.n_q_points);

  for (unsigned int cell = 0; cell < n_cells; ++cell)
    for (unsigned int q = 0; q < fe_eval.n_q_points; ++q)
      for (unsigned int i = 0;
           i < _matrix_free.n_active_entries_per_cell_batch(cell); ++i)
      {
        typename dealii::DoFHandler<dim>::cell_iterator cell_it =
            _matrix_free.get_cell_iterator(cell, i);
        // Cast to Triangulation<dim>::cell_iterator to access the material_id
        typename dealii::Triangulation<dim>::active_cell_iterator cell_tria(
            cell_it);

        _liquid_ratio(cell, q)[i] = _material_properties.get_state_ratio(
            cell_tria, MaterialState::liquid);
        _powder_ratio(cell, q)[i] = _material_properties.get_state_ratio(
            cell_tria, MaterialState::powder);
        _material_id(cell, q)[i] = cell_tria->material_id();
      }

  // If we are using boundary conditions other than adiabatic, we also need to
  // update the face variables
  if (!(_boundary_type & BoundaryType::adiabatic))
  {
    unsigned int const n_inner_faces = _matrix_free.n_inner_face_batches();
    unsigned int const n_boundary_faces =
        _matrix_free.n_boundary_face_batches();
    unsigned int const n_faces = n_inner_faces + n_boundary_faces;
    dealii::FEFaceEvaluation<dim, fe_degree, fe_degree + 1, 1, double>
        fe_face_eval(_matrix_free, true);

    _face_powder_ratio.reinit(n_faces, fe_face_eval.n_q_points);
    _face_material_id.reinit(n_faces, fe_face_eval.n_q_points);

    for (unsigned int face = 0; face < n_inner_faces; ++face)
      for (unsigned int q = 0; q < fe_face_eval.n_q_points; ++q)
        for (unsigned int i = 0;
             i < _matrix_free.n_active_entries_per_face_batch(face); ++i)
        {
          // We get the two cells associated with the face
          auto [cell_1, face_1] = _matrix_free.get_face_iterator(face, i, true);
          auto [cell_2, face_2] =
              _matrix_free.get_face_iterator(face, i, false);
          // We only care for cells that are at the boundary between activated
          // and deactivated domains
          unsigned int const active_fe_index_1 = cell_1->active_fe_index();
          unsigned int const active_fe_index_2 = cell_2->active_fe_index();
          if (active_fe_index_1 == active_fe_index_2)
          {
            continue;
          }
          // We need the cell that has FE_Q not the one that has FE_Nothing
          // Cast to Triangulation<dim>::cell_iterator to access the material_id
          typename dealii::Triangulation<dim>::active_cell_iterator cell_tria(
              (active_fe_index_1 == 0) ? cell_1 : cell_2);
          if (cell_tria->is_locally_owned())
          {
            _face_powder_ratio(face, q)[i] =
                _material_properties.get_state_ratio(cell_tria,
                                                     MaterialState::powder);
            _face_material_id(face, q)[i] = cell_tria->material_id();
          }
        }

    for (unsigned int face = n_inner_faces; face < n_faces; ++face)
      for (unsigned int q = 0; q < fe_face_eval.n_q_points; ++q)
        for (unsigned int i = 0;
             i < _matrix_free.n_active_entries_per_face_batch(face); ++i)
        {
          // We get one cell associated with the face
          auto [cell, face_] = _matrix_free.get_face_iterator(face, i, true);
          unsigned int const active_fe_index = cell->active_fe_index();
          if (active_fe_index == 1)
          {
            continue;
          }
          // We need the cell that has FE_Q not the one that has FE_Nothing
          // Cast to Triangulation<dim>::cell_iterator to access the material_id
          typename dealii::Triangulation<dim>::active_cell_iterator cell_tria(
              cell);
          if (cell_tria->is_locally_owned())
          {
            _face_powder_ratio(face, q)[i] =
                _material_properties.get_state_ratio(cell_tria,
                                                     MaterialState::powder);
            _face_material_id(face, q)[i] = cell_tria->material_id();
          }
        }
  }
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree,
                     MemorySpaceType>::set_state_to_material_properties()
{
  _material_properties.set_state(_liquid_ratio, _powder_ratio,
                                 _cell_it_to_mf_cell_map,
                                 _matrix_free.get_dof_handler());
}

template <int dim, int fe_degree, typename MemorySpaceType>
void ThermalOperator<dim, fe_degree, MemorySpaceType>::
    set_material_deposition_orientation(
        std::vector<double> const &deposition_cos,
        std::vector<double> const &deposition_sin)
{
  unsigned int const n_cells = _matrix_free.n_cell_batches();
  dealii::FEEvaluation<dim, fe_degree, fe_degree + 1, 1, double> fe_eval(
      _matrix_free);

  _deposition_cos.reinit(n_cells, fe_eval.n_q_points);
  _deposition_sin.reinit(n_cells, fe_eval.n_q_points);

  using dof_cell_iterator = typename dealii::DoFHandler<dim>::cell_iterator;
  std::map<dof_cell_iterator, unsigned int> cell_mapping;
  unsigned int pos = 0;
  for (auto const &cell : dealii::filter_iterators(
           _matrix_free.get_dof_handler().active_cell_iterators(),
           dealii::IteratorFilters::LocallyOwnedCell(),
           dealii::IteratorFilters::ActiveFEIndexEqualTo(0)))
  {
    cell_mapping[cell] = pos;
    ++pos;
  }
  ASSERT((pos == 0) || (pos - 1 < deposition_cos.size()),
         "Out-of-bound access.");

  for (unsigned int cell = 0; cell < n_cells; ++cell)
    for (unsigned int q = 0; q < fe_eval.n_q_points; ++q)
      for (unsigned int i = 0;
           i < _matrix_free.n_active_entries_per_cell_batch(cell); ++i)
      {
        dof_cell_iterator cell_it = _matrix_free.get_cell_iterator(cell, i);

        if (cell_it->active_fe_index() == 0)
        {
          unsigned int const j = cell_mapping[cell_it];
          _deposition_cos(cell, q)[i] = deposition_cos[j];
          _deposition_sin(cell, q)[i] = deposition_sin[j];
        }
      }
}

} // namespace adamantine

INSTANTIATE_DIM_FEDEGREE_HOST(TUPLE(ThermalOperator))
