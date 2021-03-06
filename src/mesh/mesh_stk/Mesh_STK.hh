/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */
// -------------------------------------------------------------
// file: Mesh_STK.hh
// -------------------------------------------------------------
/**
 * @file   Mesh_STK.hh
 * @author William A. Perkins
 * @date Thu Sep 29 10:51:22 2011
 * 
 * @brief  
 * 
 * 
 */
// -------------------------------------------------------------
// Created May  2, 2011 by William A. Perkins
// Last Change: Thu Sep 29 10:51:22 2011 by William A. Perkins <d3g096@PE10900.pnl.gov>
// -------------------------------------------------------------

// SCCS ID: $Id$ Battelle PNL

#ifndef _Mesh_STK_hh_
#define _Mesh_STK_hh_

#include <Teuchos_ParameterList.hpp>
#include <Epetra_Map.h>
#include <Epetra_CrsGraph.h>
#include <Epetra_MpiComm.h>
#include <memory>

#include "GeometricModel.hh"
#include "RegionLabeledSet.hh"
#include "VerboseObject.hh"


#include "Mesh.hh"
#include "Data_structures.hh"
#include "Entity_map.hh"

namespace Amanzi {
namespace AmanziMesh {

namespace Data {
class HexMeshGenerator;
}
class GenerationSpec;

// -------------------------------------------------------------
//  class Mesh_STK
// -------------------------------------------------------------
class Mesh_STK : public Amanzi::AmanziMesh::Mesh {
 public:

  /// Construct a mesh from a
  explicit Mesh_STK(STK::Mesh_STK_Impl_p mesh);

  // the request_faces and request_edges arguments have to be at the
  // end and not in the middle because if we omit them and specify a
  // pointer argument like gm or verbosity_obj, then there is implicit
  // conversion of the pointer to bool, thereby defeating the intent
  // of the call and making the pointer argument seem NULL. In C++11,
  // we could "delete" the illegal version of the call effectively
  // blocking the implicit conversion.
  
  /// Construct hexahedral mesh of the given size and spacing
  Mesh_STK(const Epetra_MpiComm *comm_, 
           const unsigned int& ni, const unsigned int& nj, const unsigned int& nk,
           const double& xorigin = 0.0, 
           const double& yorigin = 0.0, 
           const double& zorigin = 0.0, 
           const double& xdelta = 1.0, 
           const double& ydelta = 1.0, 
           const double& zdelta = 1.0,
           const Teuchos::RCP<const AmanziGeometry::GeometricModel>& gm = Teuchos::null,
           const Teuchos::RCP<const VerboseObject>& verbosity_obj = Teuchos::null,
           const bool request_faces = true,
           const bool request_edges = false);
  
  /// Construct hexahedral mesh (Mesh_simple alternative)
  Mesh_STK(double x0, double y0, double z0,
           double x1, double y1, double z1,
           int nx, int ny, int nz, 
           const Epetra_MpiComm *comm,
           const Teuchos::RCP<const AmanziGeometry::GeometricModel>& gm = Teuchos::null,
           const Teuchos::RCP<const VerboseObject>& verbosity_obj = Teuchos::null,
           const bool request_faces = true,
           const bool request_edges = false);

  Mesh_STK(const double x0, 
           const double y0,
           const double x1, 
           const double y1,
           const int nx, 
           const int ny, 
           const Epetra_MpiComm *comm,
           const Teuchos::RCP<const AmanziGeometry::GeometricModel>& gm = Teuchos::null,
           const Teuchos::RCP<const VerboseObject>& verbosity_obj = Teuchos::null,
           const bool request_faces = true,
           const bool request_edges = false);
  
  /// Construct a hexedral mesh from a parameter list (Mesh_simple alternative)
  Mesh_STK(Teuchos::ParameterList &parameter_list,
           const Epetra_MpiComm *comm,
           const Teuchos::RCP<const AmanziGeometry::GeometricModel>& gm = Teuchos::null,
           const Teuchos::RCP<const VerboseObject>& verbosity_obj = Teuchos::null,
           const bool request_faces = true,
           const bool request_edges = false);

  /// Construct a hexedral mesh from specs (Mesh_simple alternative)
  Mesh_STK(const GenerationSpec& gspec,
           const Epetra_MpiComm *comm,
           const Teuchos::RCP<const AmanziGeometry::GeometricModel>& gm = Teuchos::null,
           const Teuchos::RCP<const VerboseObject>& verbosity_obj = Teuchos::null,
           const bool request_faces = true,
           const bool request_edges = false);

  /// Construct a mesh from a Exodus II file or file set
  Mesh_STK(const Epetra_MpiComm *comm_, 
           const std::string& fname,
           const Teuchos::RCP<const AmanziGeometry::GeometricModel>& gm = Teuchos::null,
           const Teuchos::RCP<const VerboseObject>& verbosity_obj = Teuchos::null,
           const bool request_faces = true,
           const bool request_edges = false);

  /// Construct a mesh from a Exodus II file or file set
  Mesh_STK(const char *filename, const Epetra_MpiComm *comm,
           const Teuchos::RCP<const AmanziGeometry::GeometricModel>& gm = Teuchos::null,
           const Teuchos::RCP<const VerboseObject>& verbosity_obj = Teuchos::null,
           const bool request_faces = true,
           const bool request_edges = false);

  // Construct a mesh by extracting a subset of entities from another
  // mesh. In some cases like extracting a surface mesh from a volume
  // mesh, constructor can be asked to "flatten" the mesh to a lower
  // dimensional space or to extrude the mesh to give higher
  // dimensional cells

  Mesh_STK(const Mesh *inmesh,
           const std::vector<std::string>& setnames,
           const Entity_kind setkind,
           const bool flatten = false,
           const bool extrude = false,
           const bool request_faces = true,
           const bool request_edges = false);

  Mesh_STK(const Mesh& inmesh,
           const std::vector<std::string>& setnames,
           const Entity_kind setkind,
           const bool flatten = false,
           const bool extrude = false,
           const bool request_faces = true,
           const bool request_edges = false);

  Mesh_STK (const Mesh& inmesh, 
            const std::vector<int>& entity_id_list, 
            const Entity_kind entity_kind,
            const bool flatten = false,
            const bool extrude = false,
            const bool request_faces = true,
            const bool request_edges = false);


  /// Destructor
  ~Mesh_STK(void);

  /// Get parallel type of eneity
  Parallel_type entity_get_ptype(const Entity_kind kind, 
                                 const Entity_ID entid) const;

  // Get cell type
  Cell_type cell_get_type(const Entity_ID cellid) const;

  //
  // General mesh information
  // -------------------------
  //
  
  // Number of entities of any kind (cell, face, node) and in a
  // particular category (OWNED, GHOST, USED)
  unsigned int num_entities (const Entity_kind kind,
                             const Parallel_type ptype) const;
    
  // Global ID of any entity
  Entity_ID GID(const Entity_ID lid, const Entity_kind kind) const;
    
  /// Get entity local id (0-based) given a global id (0-based)
  Entity_ID LID(const Entity_ID& gid, const Entity_kind& kind) const;

  //
  // Mesh Entity Adjacencies 
  //-------------------------

  // Downward Adjacencies
  //---------------------

  // Get nodes of cell 
  // On a distributed mesh, all nodes (OWNED or GHOST) of the cell 
  // are returned
  // Nodes are returned in a standard order (Exodus II convention)
  // STANDARD CONVENTION WORKS ONLY FOR STANDARD CELL TYPES in 3D
  // For a general polyhedron this will return the nodes in
  // arbitrary order
  // In 2D, the nodes of the polygon will be returned in ccw order 
  // consistent with the face normal
  void cell_get_nodes(const Entity_ID cellid, 
                      Entity_ID_List *nodeids) const;

  // Get nodes of face 
  // On a distributed mesh, all nodes (OWNED or GHOST) of the face 
  // are returned
  // In 3D, the nodes of the face are returned in ccw order consistent
  // with the face normal
  // In 2D, nfnodes is 2
  void face_get_nodes(const Entity_ID faceid, 
                      Entity_ID_List *nodeids) const;


  // Get nodes of edge

  void edge_get_nodes(const Entity_ID edgeid, Entity_ID *nodeid0,
                      Entity_ID *nodeid1) const {
    Errors::Message mesg("Edges not implemented in this framework. Use MSTK");
    amanzi_throw(mesg);
  }

  // Upward adjacencies
  //-------------------
  
  // Cells of type 'ptype' connected to a node
  void node_get_cells(const Entity_ID nodeid, 
                      const Parallel_type ptype,
                      Entity_ID_List *cellids) const;
    
  // Faces of type 'ptype' connected to a node
  void node_get_faces (const Entity_ID nodeid, 
                       const Parallel_type ptype,
                       Entity_ID_List *faceids) const;
    
  // Get faces of ptype of a particular cell that are connected to the
  // given node
  void node_get_cell_faces (const Entity_ID nodeid, 
                            const Entity_ID cellid,
                            const Parallel_type ptype,
                            Entity_ID_List *faceids) const;    
    

  // Same level adjacencies
  //-----------------------

  // Face connected neighboring cells of given cell of a particular ptype
  // (e.g. a hex has 6 face neighbors)

  // The order in which the cellids are returned cannot be
  // guaranteed in general except when ptype = USED, in which case
  // the cellids will correcpond to cells across the respective
  // faces given by cell_get_faces
  void cell_get_face_adj_cells(const Entity_ID cellid,
                               const Parallel_type ptype,
                               Entity_ID_List *fadj_cellids) const;

  // Node connected neighboring cells of given cell
  // (a hex in a structured mesh has 26 node connected neighbors)
  // The cells are returned in no particular order
  void cell_get_node_adj_cells(const Entity_ID cellid,
                               const Parallel_type ptype,
                               Entity_ID_List *nadj_cellids) const;

    
  //
  // Mesh entity geometry
  //--------------
  //
    
  // Node coordinates - 3 in 3D and 2 in 2D
  void node_get_coordinates(const Entity_ID nodeid, 
                            AmanziGeometry::Point *ncoord) const;
    
    
  // Face coordinates - conventions same as face_to_nodes call 
  // Number of nodes is the vector size divided by number of spatial dimensions
  void face_get_coordinates(const Entity_ID faceid, 
                            std::vector<AmanziGeometry::Point> *fcoords) const; 
    
  // Coordinates of cells in standard order (Exodus II convention)
  // STANDARD CONVENTION WORKS ONLY FOR STANDARD CELL TYPES IN 3D
  // For a general polyhedron this will return the node coordinates in
  // arbitrary order
  // Number of nodes is vector size divided by number of spatial dimensions
  void cell_get_coordinates(const Entity_ID cellid, 
                            std::vector<AmanziGeometry::Point> *ccoords) const;


  // Modify the coordinates of a node
  void node_set_coordinates (const Entity_ID nodeid, const double *coords);

  void node_set_coordinates (const Entity_ID nodeid, const AmanziGeometry::Point coords);

  //
  // Epetra maps
  //------------
  const Epetra_Map& cell_map(bool include_ghost) const;
  const Epetra_Map& face_map(bool include_ghost) const; 
  const Epetra_Map& node_map(bool include_ghost) const;
  const Epetra_Map& exterior_face_map(bool include_ghost) const; 

  // Epetra importer that will allow apps to import values from a
  // Epetra vector defined on all owned faces into an Epetra vector
  // defined only on exterior faces
  const Epetra_Import& exterior_face_importer (void) const;
    
  //
  // Boundary Conditions or Sets
  //----------------------------

  // Get list of entities of type 'category' in set
  using Mesh::get_set_entities;

  virtual
  void get_set_entities_and_vofs(const std::string setname, 
                                 const Entity_kind kind, 
                                 const Parallel_type ptype, 
                                 std::vector<Entity_ID> *entids,
                                 std::vector<double> *vofs) const;


  int deform(const std::vector<double>& target_cell_volumes__in, 
             const std::vector<double>& min_cell_volumes__in, 
             const Entity_ID_List& fixed_nodes,
             const bool move_vertical);


  // Make a cell-to-cell graph from the mesh
  Teuchos::RCP<Epetra_CrsGraph> cellgraph(void) const;

  // Redistribute the mesh according to the specified cell map
  void redistribute(const Epetra_Map& cellmap);

  // Repartition and redistribute the mesh according to the specified parameters
  void redistribute(const Teuchos::ParameterList& paramlist=Teuchos::ParameterList("EmptyParameterList"));


  // Miscellaneous
  void write_to_exodus_file(const std::string filename) const;

 private:
  // A list of supported entity kinds
  static const unsigned int num_kinds_;
  static const Entity_kind kinds_[];

  // The mesh implementation
  STK::Mesh_STK_Impl_p mesh_;

  // Maps, Accessors and setters.
  // ----------------------------
  
  // A thing to relate Epetra_Maps to mesh entities
  typedef std::map< Entity_kind, Teuchos::RCP<Epetra_Map> > MapSet;

  MapSet map_owned_;          /**< The Epetra_Map's for owned entities */
  MapSet map_used_;           /**< The Epetra_Map's for used (owned + ghost) entities */
    
  // Build a mesh from a Exodus II file or file set
  void read_exodus_(const std::string& fname);

  // Generate a mesh with a hexahedral generator
  void generate_(Data::HexMeshGenerator& generator);

  // Generate a hexahedral mesh 
  void generate_(const unsigned int& ni, const unsigned int& nj, const unsigned int& nk,
                 const double& xorigin, 
                 const double& yorigin, 
                 const double& zorigin, 
                 const double& xdelta, 
                 const double& ydelta, 
                 const double& zdelta);

  // Generate a hexahedral mesh (with specifications)
  void generate_(const GenerationSpec& gspec);

  // Build and store the required Epetra_Map instances
  void build_maps_();

  // Get the appropriate map for the specified @c kind
  const Epetra_Map& get_map_(const Entity_kind& kind, 
                             const bool& include_ghost) const;

  // fill Mesh::tmp_setnameid_map with Cell part name/id
  // void fill_setnameid_map_(void);

  // Get faces of a cell and directions in which the cell uses the face 

  // The Amanzi coding guidelines regarding function arguments is purposely
  // violated here to allow for a default input argument

  // On a distributed mesh, this will return all the faces of the
  // cell, OWNED or GHOST. If ordered = true, the faces will be
  // returned in a standard order according to Exodus II convention
  // for standard cells; in all other situations (ordered = false or
  // non-standard cells), the list of faces will be in arbitrary order

  // In 3D, direction is 1 if face normal points out of cell
  // and -1 if face normal points into cell
  // In 2D, direction is 1 if face/edge is defined in the same
  // direction as the cell polygon, and -1 otherwise
  void cell_get_faces_and_dirs_internal_(const Entity_ID cellid,
                                         Entity_ID_List *faceids,
                                         std::vector<int> *face_dirs,
                                         const bool ordered=false) const;

  // Edges of a cell
  void cell_get_edges_internal_(const Entity_ID cellid,
                                Entity_ID_List *edgeids) const 
  { 
    Errors::Message mesg("Edges not implemented in this interface. Use MSTK");
    Exceptions::amanzi_throw(mesg);
  }

  // Edges and edge directions of a 2D cell
  void cell_2D_get_edges_and_dirs_internal_(const Entity_ID cellid,
                                            Entity_ID_List *edgeids,
                                            std::vector<int> *edgedirs) const 
  { 
    Errors::Message mesg("Edges not implemented in this interface. Use MSTK");
    Exceptions::amanzi_throw(mesg);
  }

  // Cells connected to a face
  void face_get_cells_internal_(const Entity_ID faceid, 
                                const Parallel_type ptype,
                                Entity_ID_List *cellids) const;
    
  // Edges and edge directions of a face
  void face_get_edges_and_dirs_internal_(const Entity_ID cellid,
                                         Entity_ID_List *edgeids,
                                         std::vector<int> *edgedirs,
                                         bool ordered=true) const
  {
    Errors::Message mesg("Edges not implemented in this framework. Use MSTK");
    amanzi_throw(mesg);
  };
};

}  // namespace AmanziMesh
}  // namespace Amanzi

#endif
