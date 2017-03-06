//----------------------------------*-C++-*----------------------------------//
/*!
 * \file   mesh_pass_driver.h
 * \author Alex Long
 * \date   March 3 2017
 * \brief  Functions to run IMC with two-sided mesh passing
 * \note   ***COPYRIGHT_GOES_HERE****
 */
//---------------------------------------------------------------------------//

#ifndef mesh_pass_driver_h_
#define mesh_pass_driver_h_

#include <iostream>
#include <mpi.h>
#include <functional>
#include <vector>

#include "census_creation.h"
#include "completion_manager_milagro.h"
#include "completion_manager_rma.h"
#include "mesh_rma_manager.h"
#include "mesh_request_manager.h"
#include "message_counter.h"
#include "mpi_types.h"
#include "info.h"
#include "imc_state.h"
#include "imc_parameters.h"
#include "load_balance.h"
#include "pretransport_requests.h"
#include "mesh.h"
#include "mesh_pass_transport.h"
#include "source.h"
#include "timer.h"
#include "write_silo.h"

void imc_mesh_pass_driver(Mesh *mesh, 
                          IMC_State *imc_state,
                          IMC_Parameters *imc_parameters,
                          MPI_Types *mpi_types,
                          const Info &mpi_info)
{
  using std::vector;
  vector<double> abs_E(mesh->get_global_num_cells(), 0.0);
  vector<Photon> census_photons;
  vector<uint32_t> needed_grip_ids; //! Grips needed after load balance
  Message_Counter mctr;
  int rank = mpi_info.get_rank();
  int n_rank = mpi_info.get_n_rank();

  // make object that handles requests for local and remote data
  Mesh_Request_Manager *req_manager = new Mesh_Request_Manager(rank, 
    mesh->get_off_rank_bounds(), mesh->get_global_num_cells(), 
    mesh->get_max_grip_size(), mpi_types, mesh->get_const_cells_ptr());
  req_manager->start_simulation(mctr);

  // make object that handles completion messages, completion
  // object sets up the binary tree structure
  Completion_Manager *comp;
  if (imc_parameters->get_completion_method() == Constants::RMA_COMPLETION)
    comp = new Completion_Manager_RMA(rank, n_rank);
  else
    comp = new Completion_Manager_Milagro(rank, n_rank);

  while (!imc_state->finished())
  {
    if (rank==0) imc_state->print_timestep_header();

    mctr.reset_counters();

    // set opacity, Fleck factor, all energy to source
    mesh->calculate_photon_energy(imc_state);

    // all reduce to get total source energy to make correct number of
    // particles on each rank
    double global_source_energy = mesh->get_total_photon_E();
    MPI_Allreduce(MPI_IN_PLACE, &global_source_energy, 1, MPI_DOUBLE,
      MPI_SUM, MPI_COMM_WORLD);

    // this will be zero on first time step, source construction
    // handles initial census
    imc_state->set_pre_census_E(get_photon_list_E(census_photons)); 

    // setup source and load balance, time load balance
    Source source(mesh, imc_state, imc_parameters->get_n_user_photon(),
      global_source_energy, census_photons);
    Timer t_lb;
    t_lb.start_timer("load balance");
    load_balance(source.get_work_vector(), census_photons,
      source.get_n_photon(), mpi_types, mpi_info);
    
    // get new particle count after load balance. Group particle work by cell
    source.post_lb_prepare_source();

    // prerequest data for work packets and census particles not on your rank
    pretransport_requests(source.get_work_vector(), census_photons, mesh,
      req_manager, mctr);

    imc_state->set_transported_particles(source.get_n_photon());

    // cell properties are set in calculate_photon_energy--make sure
    // everybody gets here together so that windows are not changing
    // when transport starts
    MPI_Barrier(MPI_COMM_WORLD);

    t_lb.stop_timer("load balance");
    imc_state->set_load_balance_time(t_lb.get_time("load balance"));

    vector<Cell> new_cells = req_manager->process_mesh_requests(mctr);
    if (!new_cells.empty()) mesh->add_non_local_mesh_cells(new_cells);

    // set pending receives for cell data requests and completion tracker
    comp->start_timestep(mctr);

    // transport photons
    census_photons = mesh_pass_transport(source, mesh, imc_state, 
      imc_parameters, comp, req_manager, mctr, abs_E, mpi_types, mpi_info);

    // using MPI_IN_PLACE allows the same vector to send and be overwritten
    MPI_Allreduce(MPI_IN_PLACE, &abs_E[0], mesh->get_global_num_cells(), 
      MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    mesh->update_temperature(abs_E, imc_state);

    imc_state->print_conservation(imc_parameters->get_dd_mode());

    // purge the working mesh, it will be updated by other ranks and is now 
    // invalid
    mesh->purge_working_mesh();

    // reset counters and max indices in mesh request object
    req_manager->end_timestep();

    // update time for next step
    imc_state->next_time_step();
  }

  req_manager->end_simulation(mctr);

  // delete completion manager (closes and free window in RMA version)
  delete comp;
  delete req_manager;
}

#endif // mesh_pass_driver_h_
//---------------------------------------------------------------------------//
// end of mesh_pass_driver.h
//---------------------------------------------------------------------------//
