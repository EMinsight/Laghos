#!/usr/bin/env bash

options=( 'pa' 'fa' )

parallel_refs=0
mesh_dim=2
maxL2dof=1000000

outfile=timings_${mesh_dim}d

if (( mesh_dim == 2 )); then
  nproc=4
  mesh_file=data/square01_quad.mesh
else
  nproc=8
  mesh_file=data/cube01_hex.mesh
fi

calc() { awk "BEGIN{print $*}"; }

run_case()
{
    # Pass command as all inputs
    # Outputs: order refs h1_dofs l2_dofs h1_cg_rate l2_cg_rate forces_rate update_quad_rate

    "$@" | tee run.log | awk '
BEGIN { ref = 0 }
/--refine-serial/ { ref += $2 }
/--refine-parallel/ { ref += $2 }
/--order/ { order = $2 }
/Number of kinematic/ { h1_dofs = $7 }
/Number of specific internal energy/ { l2_dofs = $7 }
/CG \(H1\) rate/ { h1_cg_rate = $9 }
/CG \(L2\) rate/ { l2_cg_rate = $9 }
/Forces rate/ { forces_rate = $8 }
/UpdateQuadData rate/ { update_quad_rate = $8 }
/Major kernels total rate/ { total_time = $11 }
END { printf("%d %d %d %d %.8f %.8f %.8f %.8f %.8f\n", order, ref, h1_dofs, l2_dofs, h1_cg_rate, l2_cg_rate, forces_rate, update_quad_rate, total_time) }'
}

[ -r $outfile ] && cp $outfile $outfile.bak
echo "# order refs h1_dofs l2_dofs h1_cg_rate l2_cg_rate forces_rate update_quad_rate total_time" > $outfile"_"${options[0]}
echo "# order refs h1_dofs l2_dofs h1_cg_rate l2_cg_rate forces_rate update_quad_rate total_time" > $outfile"_"${options[1]}
for method in "${options[@]}"; do
  for torder in {0..4}; do
    for sref in {0..10}; do
       nzones=$(( (2**mesh_dim)**(sref+1) ))
       nL2dof=$(( nzones*(torder+1)**mesh_dim ))
       if (( nproc <= nzones )) && (( nL2dof < maxL2dof )) ; then
         echo "np"$nproc "Q"$((torder+1))"Q"$torder $sref"ref" $method $outfile"_"${options[0]}
         echo $(run_case mpirun -np $nproc ./laghos -$method -p 1 -tf 0.8 \
                       --max_steps 10 \
                       --mesh $mesh_file \
                       --refine-serial $sref \
                       --refine-parallel $parallel_refs \
                       --order-thermo $torder \
                       --order-kinematic $((torder+1))) >> $outfile"_"$method
      fi
    done
  done
done
