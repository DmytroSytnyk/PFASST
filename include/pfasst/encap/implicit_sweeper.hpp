
#ifndef _PFASST_ENCAP_IMPLICIT_SWEEPER_HPP_
#define _PFASST_ENCAP_IMPLICIT_SWEEPER_HPP_

#include <cstdlib>
#include <cassert>
#include <vector>
#include <memory>

#include "../globals.hpp"
#include "../quadrature.hpp"
#include "encapsulation.hpp"
#include "encap_sweeper.hpp"
#include "vector.hpp"

using namespace std;

namespace pfasst
{

  template<typename scalar>
  using lu_pair = pair< Matrix<scalar>, Matrix<scalar> >;

  template<typename scalar>
  static lu_pair<scalar> lu_decomposition(const Matrix<scalar>& A)
  {
    assert(A.rows() == A.cols());

    auto n = A.rows();

    Matrix<scalar> L = Matrix<scalar>::Zero(n, n);
    Matrix<scalar> U = Matrix<scalar>::Zero(n, n);

    if (A.rows() == 1) {

      L(0, 0) = 1.0;
      U(0, 0) = A(0,0);

    } else {

      // first row of U is first row of A
      auto U12 = A.block(0, 1, 1, n-1);

      // first column of L is first column of A / a11
      auto L21 = A.block(1, 0, n-1, 1) / A(0, 0);

      // remove first row and column and recurse
      auto A22  = A.block(1, 1, n-1, n-1);
      Matrix<scalar> tmp = A22 - L21 * U12;
      auto LU22 = lu_decomposition(tmp);

      L(0, 0) = 1.0;
      U(0, 0) = A(0, 0);
      L.block(1, 0, n-1, 1) = L21;
      U.block(0, 1, 1, n-1) = U12;
      L.block(1, 1, n-1, n-1) = get<0>(LU22);
      U.block(1, 1, n-1, n-1) = get<1>(LU22);

    }

    return lu_pair<scalar>(L, U);
  }

  namespace encap
  {
    using pfasst::encap::Encapsulation;

    /**
     * Implicit sweeper.
     *
     * @tparam time precision type of the time dimension
     */
    template<typename time = time_precision>
    class ImplicitSweeper
      : public pfasst::encap::EncapSweeper<time>
    {
      protected:
        //! @{
        /**
         * Node-to-node integrals of \\( F(t,u) \\) at all time nodes of the current iteration.
         */
        vector<shared_ptr<Encapsulation<time>>> s_integrals;

        /**
         * FAS corrections \\( \\tau \\) at all time nodes of the current iteration.
         */
        vector<shared_ptr<Encapsulation<time>>> fas_corrections;

        /**
         * Values of the implicit part of the right hand side \\( F_{impl}(t,u) \\) at all time nodes of the current
         * iteration.
         */
        vector<shared_ptr<Encapsulation<time>>> fs_impl;
        //! @}

        Matrix<time> q_tilde;

        /**
        * Set end state to \\( U_0 + \\int F_{expl} + F_{expl} \\).
        */
        void set_end_state()
        {
          if (this->quadrature->right_is_node()) {
            this->end_state->copy(this->state.back());
          } else {
            vector<shared_ptr<Encapsulation<time>>> dst = { this->end_state };
            dst[0]->copy(this->start_state);
            dst[0]->mat_apply(dst, this->get_controller()->get_time_step(), this->quadrature->get_b_mat(), this->fs_impl, false);
          }
        }

        /**
        * Augment nodes: nodes <- [t0] + dt * nodes
        */
        vector<time> augment(time t0, time dt, vector<time> const & nodes)
        {
          vector<time> t(1 + nodes.size());
          t[0] = t0;
          for (size_t m = 0; m < nodes.size(); m++) {
            t[m+1] = t0 + dt * nodes[m];
          }
          return t;
        }

      public:
        //! @{
        ImplicitSweeper() = default;
        virtual ~ImplicitSweeper() = default;
        //! @}

        //! @{
        /**
         * @copydoc ISweeper::setup(bool)
         *
         */
        virtual void setup(bool coarse) override
        {
          pfasst::encap::EncapSweeper<time>::setup(coarse);

          auto const nodes = this->quadrature->get_nodes();
          auto const num_nodes = this->quadrature->get_num_nodes();

          if (this->quadrature->left_is_node()) {
            CLOG(INFO, "Sweeper") << "implicit sweeper shouldn't include left endpoint";
            throw ValueError("implicit sweeper shouldn't include left endpoint");
          }

          for (size_t m = 0; m < num_nodes; m++) {
            this->s_integrals.push_back(this->get_factory()->create(pfasst::encap::solution));
            this->fs_impl.push_back(this->get_factory()->create(pfasst::encap::function));
          }

          Matrix<time> QT = this->quadrature->get_q_mat().transpose();
          auto lu = lu_decomposition(QT);
          auto L = get<0>(lu);
          auto U = get<1>(lu);
          this->q_tilde = U.transpose();

          CLOG(DEBUG, "Sweeper") << "Q':" << endl << QT;
          CLOG(DEBUG, "Sweeper") << "L:" << endl << L;
          CLOG(DEBUG, "Sweeper") << "U:" << endl << U;
          CLOG(DEBUG, "Sweeper") << "LU:" << endl << L * U;
          CLOG(DEBUG, "Sweeper") << "q_tilde:" << endl << this->q_tilde;
        }

        /**
         * Compute low-order provisional solution.
         *
         * This performs forward/backward Euler steps across the nodes to compute a low-order provisional solution.
         *
         * @param[in] initial if `true` the explicit and implicit part of the right hand side of the
         *     ODE get evaluated with the initial value
         */
        virtual void predict(bool initial) override
        {
          UNUSED(initial);

          auto const dt = this->get_controller()->get_time_step();
          auto const t  = this->get_controller()->get_time();

          CLOG(INFO, "Sweeper") << "predicting step " << this->get_controller()->get_step() + 1
                                << " (t=" << t << ", dt=" << dt << ")";

          auto const anodes = augment(t, dt, this->quadrature->get_nodes());
          for (size_t m = 0; m < anodes.size() - 1; ++m) {
            this->impl_solve(this->fs_impl[m], this->state[m], anodes[m], anodes[m+1] - anodes[m],
                             m == 0 ? this->get_start_state() : this->state[m-1]);
          }

          this->set_end_state();
        }

        /**
         * Perform one SDC sweep/iteration.
         *
         * This computes a high-order solution from the previous iteration's function values and
         * corrects it using forward/backward Euler steps across the nodes.
         */
        virtual void sweep() override
        {
          auto const dt = this->get_controller()->get_time_step();
          auto const t  = this->get_controller()->get_time();

          CLOG(INFO, "Sweeper") << "sweeping on step " << this->get_controller()->get_step() + 1
                                << " in iteration " << this->get_controller()->get_iteration()
                                << " (dt=" << dt << ")";

          this->s_integrals[0]->mat_apply(this->s_integrals, dt, this->quadrature->get_s_mat(), this->fs_impl, true);
          if (this->fas_corrections.size() > 0) {
            for (size_t m = 0; m < this->s_integrals.size(); m++) {
              this->s_integrals[m]->saxpy(1.0, this->fas_corrections[m]);
            }
          }

          for (size_t m = 0; m < this->s_integrals.size(); m++) {
            for (size_t n = 0; n < m; n++) {
              this->s_integrals[m]->saxpy(-dt*this->q_tilde(m, n), this->fs_impl[n]);
            }
          }

          shared_ptr<Encapsulation<time>> rhs = this->get_factory()->create(pfasst::encap::solution);

          auto const anodes = augment(t, dt, this->quadrature->get_nodes());
          for (size_t m = 0; m < anodes.size() - 1; ++m) {
            auto const ds = anodes[m+1] - anodes[m];
            rhs->copy(m == 0 ? this->get_start_state() : this->state[m-1]);
            rhs->saxpy(1.0, this->s_integrals[m]);
            rhs->saxpy(-ds, this->fs_impl[m]);
            for (size_t n = 0; n < m; n++) {
              rhs->saxpy(dt*this->q_tilde(m, n), this->fs_impl[n]);
            }
            this->impl_solve(this->fs_impl[m], this->state[m], anodes[m], ds, rhs);
          }
          this->set_end_state();
        }

        /**
         * Advance the end solution to start solution.
         */
        virtual void advance() override
        {
          this->start_state->copy(this->end_state);
        }

        /**
         * @copybrief EncapSweeper::evaluate()
         */
        virtual void reevaluate(bool initial_only) override
        {
          if (initial_only) {
            return;
          }
          auto const dt = this->get_controller()->get_time_step();
          auto const t0 = this->get_controller()->get_time();
          auto const nodes = this->quadrature->get_nodes();
          for (size_t m = 0; m < nodes.size(); m++) {
            this->f_impl_eval(this->fs_impl[m], this->state[m], t0 + dt * nodes[m]);
          }
        }

        /**
         * @copybrief EncapSweeper::integrate()
         *
         * @param[in] dt width of time interval to integrate over
         * @param[in,out] dst integrated values; will get zeroed out beforehand
         */
        virtual void integrate(time dt, vector<shared_ptr<Encapsulation<time>>> dst) const override
        {
          dst[0]->mat_apply(dst, dt, this->quadrature->get_q_mat(), this->fs_impl, true);
        }
        //! @}

        //! @{
        /**
         * Evaluate the implicit part of the ODE.
         *
         * This is typically called to compute the implicit part of the right hand side at the first
         * collocation node, and on all nodes after restriction or interpolation.
         *
         * @param[in,out] f_impl_encap Encapsulation to store the implicit function evaluation.
         * @param[in] u_encap Encapsulation storing the solution state at which to evaluate the
         *     implicit part of the ODE.
         * @param[in] t Time point of the evaluation.
         *
         * @note This method must be implemented in derived sweepers.
         */
        virtual void f_impl_eval(shared_ptr<Encapsulation<time>> f_impl_encap,
                                 shared_ptr<Encapsulation<time>> u_encap,
                                 time t)
        {
          UNUSED(f_impl_encap); UNUSED(u_encap); UNUSED(t);
          throw NotImplementedYet("implicit (f_impl_eval)");
        }

        /**
         * Solve \\( U - \\Delta t F_{\\rm impl}(U) = RHS \\) for \\( U \\).
         *
         * During an implicit SDC sweep, the correction equation is evolved using a backward-Euler
         * stepper.  This routine (implemented by the user) performs the solve required to perform
         * one backward-Euler sub-step, and also returns \\( F_{\\rm impl}(U) \\).
         *
         * @param[in,out] f_encap Encapsulation to store the evaluated implicit piece.
         * @param[in,out] u_encap Encapsulation to store the solution of the backward-Euler sub-step.
         * @param[in] t time point (of \\( RHS \\)).
         * @param[in] dt sub-step size to the previous time point (\\( \\Delta t \\)).
         * @param[in] rhs_encap Encapsulation that stores \\( RHS \\).
         *
         * @note This method must be implemented in derived sweepers.
         */
        virtual void impl_solve(shared_ptr<Encapsulation<time>> f_encap,
                                shared_ptr<Encapsulation<time>> u_encap,
                                time t, time dt,
                                shared_ptr<Encapsulation<time>> rhs_encap)
        {
          UNUSED(f_encap); UNUSED(u_encap); UNUSED(t); UNUSED(dt); UNUSED(rhs_encap);
          throw NotImplementedYet("implicit (impl_solve)");
        }
        //! @}
    };

  }  // ::pfasst::encap
}  // ::pfasst

#endif
