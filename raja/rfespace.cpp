// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.
#include "raja.hpp"

namespace mfem {

  // ***************************************************************************
  static void offsetsFlush(const int N, int *offsets){
    forall(i,N,offsets[i] = 0;); 
  }
   
  // ***************************************************************************
  static void offsetFill(const int elements,
                         const int localDofs,
                         const int *elementMap,
                         int *offsets){
    forall(e,elements,{   
        for (int d = 0; d < localDofs; ++d) {
          const int gid = elementMap[localDofs*e + d];
          ++offsets[gid + 1];
        }
      });
  }
  
  // ***************************************************************************
  static void offsetsAggregate(const int N, int *offsets){
    // Only one thread can aggregate here
    forall(solo,1,for(int i=0;i<N;i++) offsets[i]+=offsets[i-1];);
  }

  // ***************************************************************************
  static void fillIndicesAndMap(const int elements,
                                const int localDofs,
                                const bool dof_map_is_identity,
                                const int *dof_map,
                                const int *elementMap,
                                int *offsets,
                                int *indices,
                                int *map){
   forall(e, elements, {
        for (int d = 0; d < localDofs; ++d) {
          const int did = dof_map_is_identity?d:dof_map[d];
          const int gid = elementMap[localDofs*e + did];
          const int lid = localDofs*e + d;
          indices[offsets[gid]++] = lid;
          map[lid] = gid;
        }
      });
  }
  
  // ***************************************************************************
  static void offsetsShift(const int globalDofs, int *offsets){
    forall(dummy,1,{
        for (int i = globalDofs; i > 0; --i) {
          offsets[i] = offsets[i - 1];
        }
        offsets[0] = 0;
      });
  }

// ***************************************************************************
// * RajaFiniteElementSpace
// ***************************************************************************
RajaFiniteElementSpace::RajaFiniteElementSpace(Mesh* mesh,
                                               const FiniteElementCollection* fec,
                                               const int vdim_,
                                               Ordering::Type ordering_)
  :ParFiniteElementSpace(dynamic_cast<ParMesh*>(mesh),fec,vdim_,ordering_),
   globalDofs(GetNDofs()),
   localDofs(GetFE(0)->GetDof()),
   offsets(globalDofs+1),
   indices(localDofs, GetNE()),  
   map(localDofs, GetNE()) { 
  const FiniteElement *fe = GetFE(0);
  const TensorBasisElement* el = dynamic_cast<const TensorBasisElement*>(fe);
  const Array<int> &dof_map = el->GetDofMap();
  const size_t dmSize = dof_map.Size();
#ifdef __NVCC__
  int *d_dof_map=(int*) rmalloc<int>::HoDNew(dmSize);
  checkCudaErrors(cudaMemcpy(d_dof_map,dof_map,dmSize*sizeof(int),cudaMemcpyHostToDevice));
#else
  const int *d_dof_map=dof_map;
#endif
  const bool dof_map_is_identity = (dof_map.Size()==0);
    
  const int elements = GetNE();
  const Table& e2dTable = GetElementToDofTable();
  const int* elementMap = e2dTable.GetJ();

  const size_t eMapSize = elements*localDofs;
#ifdef __NVCC__
  int *d_elementMap=(int*) rmalloc<int>::HoDNew(eMapSize);
  checkCudaErrors(cudaMemcpy(d_elementMap,elementMap,eMapSize*sizeof(int),cudaMemcpyHostToDevice));
#else
  const int *d_elementMap=elementMap;
#endif 

  // We'll be keeping a count of how many local nodes point to its global dof
  offsetsFlush(globalDofs+1,offsets.ptr());
  offsetFill(elements,localDofs,d_elementMap,offsets);
    
  // Aggregate to find offsets for each global dof
  offsetsAggregate(globalDofs,offsets.ptr()+1);  

  // For each global dof, fill in all local nodes that point to it
  fillIndicesAndMap(elements,localDofs,
                    dof_map_is_identity,
                    d_dof_map,
                    d_elementMap,
                    offsets,indices,map);

  // We shifted the offsets vector by 1 by using it as a counter
  // Now we shift it back.
  offsetsShift(globalDofs,offsets);

  const SparseMatrix* R = GetRestrictionMatrix(); assert(R);
  const Operator* P = GetProlongationMatrix(); assert(P);
  
  const int mHeight = R->Height();
  const int* I = R->GetI();
  const int* J = R->GetJ();
  int trueCount = 0;
  for (int i = 0; i < mHeight; ++i) {
    trueCount += ((I[i + 1] - I[i]) == 1);
  }
  
  Array<int> h_reorderIndices(2*trueCount);
  for (int i = 0, trueIdx=0; i < mHeight; ++i) {
    if ((I[i + 1] - I[i]) == 1) {
      h_reorderIndices[trueIdx++] = J[I[i]];
      h_reorderIndices[trueIdx++] = i;
    }
  }
  
  reorderIndices = ::new RajaArray<int>(2*trueCount);
#ifdef __NVCC__
  checkCudaErrors(cudaMemcpy(reorderIndices->ptr(),h_reorderIndices,2*trueCount*sizeof(int),cudaMemcpyHostToDevice));
#else
  ::memcpy(reorderIndices->ptr(),h_reorderIndices.GetData(),2*trueCount*sizeof(int));
#endif
  
  restrictionOp = new RajaRestrictionOperator(R->Height(),
                                              R->Width(),
                                              reorderIndices);
  prolongationOp = new RajaProlongationOperator(P);
}

// ***************************************************************************
RajaFiniteElementSpace::~RajaFiniteElementSpace() {
  ::delete restrictionOp;
  ::delete prolongationOp;
  ::delete reorderIndices;
}

// ***************************************************************************
bool RajaFiniteElementSpace::hasTensorBasis() const {
  assert(dynamic_cast<const TensorBasisElement*>(GetFE(0)));
  return true;
}

// ***************************************************************************
void RajaFiniteElementSpace::GlobalToLocal(const RajaVector& globalVec,
                                           RajaVector& localVec) const {
  const int vdim = GetVDim();
  const int localEntries = localDofs * GetNE();
  const bool vdim_ordering = ordering == Ordering::byVDIM;
  rGlobalToLocal(vdim,
                 vdim_ordering,
                 globalDofs,
                 localEntries,
                 offsets,
                 indices,
                 globalVec,
                 localVec);
}

// ***************************************************************************
// Aggregate local node values to their respective global dofs
void RajaFiniteElementSpace::LocalToGlobal(const RajaVector& localVec,
                                           RajaVector& globalVec) const {
  const int vdim = GetVDim();
  const int localEntries = localDofs * GetNE();
  const bool vdim_ordering = ordering == Ordering::byVDIM;
  rLocalToGlobal(vdim,
                 vdim_ordering,
                 globalDofs,
                 localEntries,
                 offsets,
                 indices,
                 localVec,
                 globalVec);
}
  
} // namespace mfem
