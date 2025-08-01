/**
 * Copyright 2014-2025, XGBoost Contributors
 *
 * \brief model structure for tree
 * \author Tianqi Chen
 */
#ifndef XGBOOST_TREE_MODEL_H_
#define XGBOOST_TREE_MODEL_H_

#include <xgboost/base.h>
#include <xgboost/data.h>
#include <xgboost/feature_map.h>
#include <xgboost/linalg.h>  // for VectorView
#include <xgboost/logging.h>
#include <xgboost/model.h>
#include <xgboost/multi_target_tree_model.h>  // for MultiTargetTree

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>  // for make_unique
#include <stack>
#include <string>
#include <vector>

namespace xgboost {
class Json;

/** @brief meta parameters of the tree */
struct TreeParam {
  /** @brief The number of nodes */
  bst_node_t num_nodes{1};
  /** @brief The number of deleted nodes */
  bst_node_t num_deleted{0};
  /** @brief The number of features used for tree construction */
  bst_feature_t num_feature{0};
  /** @brief leaf vector size. Used by the vector leaf. */
  bst_target_t size_leaf_vector{1};

  bool operator==(const TreeParam& b) const {
    return num_nodes == b.num_nodes && num_deleted == b.num_deleted &&
           num_feature == b.num_feature && size_leaf_vector == b.size_leaf_vector;
  }

  void FromJson(Json const& in);
  void ToJson(Json* p_out) const;
};

/*! \brief node statistics used in regression tree */
struct RTreeNodeStat {
  /*! \brief loss change caused by current split */
  bst_float loss_chg;
  /*! \brief sum of hessian values, used to measure coverage of data */
  bst_float sum_hess;
  /*! \brief weight of current node */
  bst_float base_weight;
  /*! \brief number of child that is leaf node known up to now */
  int leaf_child_cnt {0};

  RTreeNodeStat() = default;
  RTreeNodeStat(float loss_chg, float sum_hess, float weight) :
      loss_chg{loss_chg}, sum_hess{sum_hess}, base_weight{weight} {}
  bool operator==(const RTreeNodeStat& b) const {
    return loss_chg == b.loss_chg && sum_hess == b.sum_hess &&
           base_weight == b.base_weight && leaf_child_cnt == b.leaf_child_cnt;
  }
};

/**
 * \brief Helper for defining copyable data structure that contains unique pointers.
 */
template <typename T>
class CopyUniquePtr {
  std::unique_ptr<T> ptr_{nullptr};

 public:
  CopyUniquePtr() = default;
  CopyUniquePtr(CopyUniquePtr const& that) {
    ptr_.reset(nullptr);
    if (that.ptr_) {
      ptr_ = std::make_unique<T>(*that);
    }
  }
  T* get() const noexcept { return ptr_.get(); }  // NOLINT

  T& operator*() { return *ptr_; }
  T* operator->() noexcept { return this->get(); }

  T const& operator*() const { return *ptr_; }
  T const* operator->() const noexcept { return this->get(); }

  explicit operator bool() const { return static_cast<bool>(ptr_); }
  bool operator!() const { return !ptr_; }
  void reset(T* ptr) { ptr_.reset(ptr); }  // NOLINT
};

/**
 * \brief define regression tree to be the most common tree model.
 *
 *  This is the data structure used in xgboost's major tree models.
 */
class RegTree : public Model {
 public:
  using SplitCondT = bst_float;
  static constexpr bst_node_t kInvalidNodeId{MultiTargetTree::InvalidNodeId()};
  static constexpr uint32_t kDeletedNodeMarker = std::numeric_limits<uint32_t>::max();
  static constexpr bst_node_t kRoot{0};

  /*! \brief tree node */
  class Node {
   public:
    XGBOOST_DEVICE Node()  {
      // assert compact alignment
      static_assert(sizeof(Node) == 4 * sizeof(int) + sizeof(Info), "Node: 64 bit align");
    }
    Node(int32_t cleft, int32_t cright, int32_t parent, uint32_t split_ind, float split_cond,
         bool default_left)
        : parent_{parent}, cleft_{cleft}, cright_{cright} {
      this->SetParent(parent_);
      this->SetSplit(split_ind, split_cond, default_left);
    }

    /*! \brief index of left child */
    [[nodiscard]] XGBOOST_DEVICE int LeftChild() const { return this->cleft_; }
    /*! \brief index of right child */
    [[nodiscard]] XGBOOST_DEVICE int RightChild() const { return this->cright_; }
    /*! \brief index of default child when feature is missing */
    [[nodiscard]] XGBOOST_DEVICE int DefaultChild() const {
      return this->DefaultLeft() ? this->LeftChild() : this->RightChild();
    }
    /*! \brief feature index of split condition */
    [[nodiscard]] XGBOOST_DEVICE bst_feature_t SplitIndex() const {
      static_assert(!std::is_signed_v<bst_feature_t>);
      return sindex_ & ((1U << 31) - 1U);
    }
    /*! \brief when feature is unknown, whether goes to left child */
    [[nodiscard]] XGBOOST_DEVICE bool DefaultLeft() const { return (sindex_ >> 31) != 0; }
    /*! \brief whether current node is leaf node */
    [[nodiscard]] XGBOOST_DEVICE bool IsLeaf() const { return cleft_ == kInvalidNodeId; }
    /*! \return get leaf value of leaf node */
    [[nodiscard]] XGBOOST_DEVICE float LeafValue() const { return (this->info_).leaf_value; }
    /*! \return get split condition of the node */
    [[nodiscard]] XGBOOST_DEVICE SplitCondT SplitCond() const { return (this->info_).split_cond; }
    /*! \brief get parent of the node */
    [[nodiscard]] XGBOOST_DEVICE int Parent() const { return parent_ & ((1U << 31) - 1); }
    /*! \brief whether current node is left child */
    [[nodiscard]] XGBOOST_DEVICE bool IsLeftChild() const { return (parent_ & (1U << 31)) != 0; }
    /*! \brief whether this node is deleted */
    [[nodiscard]] XGBOOST_DEVICE bool IsDeleted() const { return sindex_ == kDeletedNodeMarker; }
    /*! \brief whether current node is root */
    [[nodiscard]] XGBOOST_DEVICE bool IsRoot() const { return parent_ == kInvalidNodeId; }
    /*!
     * \brief set the left child
     * \param nid node id to right child
     */
    XGBOOST_DEVICE void SetLeftChild(int nid) {
      this->cleft_ = nid;
    }
    /*!
     * \brief set the right child
     * \param nid node id to right child
     */
    XGBOOST_DEVICE void SetRightChild(int nid) {
      this->cright_ = nid;
    }
    /*!
     * \brief set split condition of current node
     * \param split_index feature index to split
     * \param split_cond  split condition
     * \param default_left the default direction when feature is unknown
     */
    XGBOOST_DEVICE void SetSplit(unsigned split_index, SplitCondT split_cond,
                                 bool default_left = false) {
      if (default_left) split_index |= (1U << 31);
      this->sindex_ = split_index;
      (this->info_).split_cond = split_cond;
    }
    /*!
     * \brief set the leaf value of the node
     * \param value leaf value
     * \param right right index, could be used to store
     *        additional information
     */
    XGBOOST_DEVICE void SetLeaf(bst_float value, int right = kInvalidNodeId) {
      (this->info_).leaf_value = value;
      this->cleft_ = kInvalidNodeId;
      this->cright_ = right;
    }
    /*! \brief mark that this node is deleted */
    XGBOOST_DEVICE void MarkDelete() {
      this->sindex_ = kDeletedNodeMarker;
    }
    /*! \brief Reuse this deleted node. */
    XGBOOST_DEVICE void Reuse() {
      this->sindex_ = 0;
    }
    // set parent
    XGBOOST_DEVICE void SetParent(int pidx, bool is_left_child = true) {
      if (is_left_child) pidx |= (1U << 31);
      this->parent_ = pidx;
    }
    bool operator==(const Node& b) const {
      return parent_ == b.parent_ && cleft_ == b.cleft_ &&
             cright_ == b.cright_ && sindex_ == b.sindex_ &&
             info_.leaf_value == b.info_.leaf_value;
    }

   private:
    /*!
     * \brief in leaf node, we have weights, in non-leaf nodes,
     *        we have split condition
     */
    union Info{
      bst_float leaf_value;
      SplitCondT split_cond;
    };
    // pointer to parent, highest bit is used to
    // indicate whether it's a left child or not
    int32_t parent_{kInvalidNodeId};
    // pointer to left, right
    int32_t cleft_{kInvalidNodeId}, cright_{kInvalidNodeId};
    // split feature index, left split or right split depends on the highest bit
    uint32_t sindex_{0};
    // extra info
    Info info_;
  };

  /*!
   * \brief change a non leaf node to a leaf node, delete its children
   * \param rid node id of the node
   * \param value new leaf value
   */
  void ChangeToLeaf(int rid, bst_float value) {
    CHECK(nodes_[nodes_[rid].LeftChild() ].IsLeaf());
    CHECK(nodes_[nodes_[rid].RightChild()].IsLeaf());
    this->DeleteNode(nodes_[rid].LeftChild());
    this->DeleteNode(nodes_[rid].RightChild());
    nodes_[rid].SetLeaf(value);
  }
  /*!
   * \brief collapse a non leaf node to a leaf node, delete its children
   * \param rid node id of the node
   * \param value new leaf value
   */
  void CollapseToLeaf(int rid, bst_float value) {
    if (nodes_[rid].IsLeaf()) return;
    if (!nodes_[nodes_[rid].LeftChild() ].IsLeaf()) {
      CollapseToLeaf(nodes_[rid].LeftChild(), 0.0f);
    }
    if (!nodes_[nodes_[rid].RightChild() ].IsLeaf()) {
      CollapseToLeaf(nodes_[rid].RightChild(), 0.0f);
    }
    this->ChangeToLeaf(rid, value);
  }

  RegTree() {
    nodes_.resize(param_.num_nodes);
    stats_.resize(param_.num_nodes);
    split_types_.resize(param_.num_nodes, FeatureType::kNumerical);
    split_categories_segments_.resize(param_.num_nodes);
    for (int i = 0; i < param_.num_nodes; i++) {
      nodes_[i].SetLeaf(0.0f);
      nodes_[i].SetParent(kInvalidNodeId);
    }
  }
  /**
   * \brief Constructor that initializes the tree model with shape.
   */
  explicit RegTree(bst_target_t n_targets, bst_feature_t n_features) : RegTree{} {
    param_.num_feature = n_features;
    param_.size_leaf_vector = n_targets;
    if (n_targets > 1) {
      this->p_mt_tree_.reset(new MultiTargetTree{&param_});
    }
  }

  /*! \brief get node given nid */
  Node& operator[](int nid) {
    return nodes_[nid];
  }
  /*! \brief get node given nid */
  const Node& operator[](int nid) const {
    return nodes_[nid];
  }

  /*! \brief get const reference to nodes */
  [[nodiscard]] const std::vector<Node>& GetNodes() const { return nodes_; }

  /*! \brief get const reference to stats */
  [[nodiscard]] const std::vector<RTreeNodeStat>& GetStats() const { return stats_; }

  /*! \brief get node statistics given nid */
  RTreeNodeStat& Stat(int nid) {
    return stats_[nid];
  }
  /*! \brief get node statistics given nid */
  [[nodiscard]] const RTreeNodeStat& Stat(int nid) const {
    return stats_[nid];
  }

  void LoadModel(Json const& in) override;
  void SaveModel(Json* out) const override;

  bool operator==(const RegTree& b) const {
    return nodes_ == b.nodes_ && stats_ == b.stats_ &&
           deleted_nodes_ == b.deleted_nodes_ && param_ == b.param_;
  }
  /* \brief Iterate through all nodes in this tree.
   *
   * \param Function that accepts a node index, and returns false when iteration should
   *        stop, otherwise returns true.
   */
  template <typename Func> void WalkTree(Func func) const {
    std::stack<bst_node_t> nodes;
    nodes.push(kRoot);
    auto &self = *this;
    while (!nodes.empty()) {
      auto nidx = nodes.top();
      nodes.pop();
      if (!func(nidx)) {
        return;
      }
      auto left = self.LeftChild(nidx);
      auto right = self.RightChild(nidx);
      if (left != RegTree::kInvalidNodeId) {
        nodes.push(left);
      }
      if (right != RegTree::kInvalidNodeId) {
        nodes.push(right);
      }
    }
  }
  /*!
   * \brief Compares whether 2 trees are equal from a user's perspective.  The equality
   *        compares only non-deleted nodes.
   *
   * \param b The other tree.
   */
  [[nodiscard]] bool Equal(const RegTree& b) const;

  /**
   * \brief Expands a leaf node into two additional leaf nodes.
   *
   * \param nid               The node index to expand.
   * \param split_index       Feature index of the split.
   * \param split_value       The split condition.
   * \param default_left      True to default left.
   * \param base_weight       The base weight, before learning rate.
   * \param left_leaf_weight  The left leaf weight for prediction, modified by learning rate.
   * \param right_leaf_weight The right leaf weight for prediction, modified by learning rate.
   * \param loss_change       The loss change.
   * \param sum_hess          The sum hess.
   * \param left_sum          The sum hess of left leaf.
   * \param right_sum         The sum hess of right leaf.
   * \param leaf_right_child  The right child index of leaf, by default kInvalidNodeId,
   *                          some updaters use the right child index of leaf as a marker
   */
  void ExpandNode(bst_node_t nid, unsigned split_index, bst_float split_value,
                  bool default_left, bst_float base_weight,
                  bst_float left_leaf_weight, bst_float right_leaf_weight,
                  bst_float loss_change, float sum_hess, float left_sum,
                  float right_sum,
                  bst_node_t leaf_right_child = kInvalidNodeId);
  /**
   * \brief Expands a leaf node into two additional leaf nodes for a multi-target tree.
   */
  void ExpandNode(bst_node_t nidx, bst_feature_t split_index, float split_cond, bool default_left,
                  linalg::VectorView<float const> base_weight,
                  linalg::VectorView<float const> left_weight,
                  linalg::VectorView<float const> right_weight);

  /**
   * \brief Expands a leaf node with categories
   *
   * \param nid               The node index to expand.
   * \param split_index       Feature index of the split.
   * \param split_cat         The bitset containing categories
   * \param default_left      True to default left.
   * \param base_weight       The base weight, before learning rate.
   * \param left_leaf_weight  The left leaf weight for prediction, modified by learning rate.
   * \param right_leaf_weight The right leaf weight for prediction, modified by learning rate.
   * \param loss_change       The loss change.
   * \param sum_hess          The sum hess.
   * \param left_sum          The sum hess of left leaf.
   * \param right_sum         The sum hess of right leaf.
   */
  void ExpandCategorical(bst_node_t nid, bst_feature_t split_index,
                         common::Span<const uint32_t> split_cat, bool default_left,
                         bst_float base_weight, bst_float left_leaf_weight,
                         bst_float right_leaf_weight, bst_float loss_change, float sum_hess,
                         float left_sum, float right_sum);
  /**
   * \brief Whether this tree has categorical split.
   */
  [[nodiscard]] bool HasCategoricalSplit() const { return !split_categories_.empty(); }
  /**
   * \brief Whether this is a multi-target tree.
   */
  [[nodiscard]] bool IsMultiTarget() const { return static_cast<bool>(p_mt_tree_); }
  /**
   * \brief The size of leaf weight.
   */
  [[nodiscard]] bst_target_t NumTargets() const { return param_.size_leaf_vector; }
  /**
   * \brief Get the underlying implementaiton of multi-target tree.
   */
  [[nodiscard]] auto GetMultiTargetTree() const {
    CHECK(IsMultiTarget());
    return p_mt_tree_.get();
  }
  /**
   * \brief Get the number of features.
   */
  [[nodiscard]] bst_feature_t NumFeatures() const noexcept { return param_.num_feature; }
  /**
   * \brief Get the total number of nodes including deleted ones in this tree.
   */
  [[nodiscard]] bst_node_t NumNodes() const noexcept { return param_.num_nodes; }
  /**
   * \brief Get the total number of valid nodes in this tree.
   */
  [[nodiscard]] bst_node_t NumValidNodes() const noexcept {
    return param_.num_nodes - param_.num_deleted;
  }
  /**
   * \brief number of extra nodes besides the root
   */
  [[nodiscard]] bst_node_t NumExtraNodes() const noexcept {
    return param_.num_nodes - 1 - param_.num_deleted;
  }
  /* \brief Count number of leaves in tree. */
  [[nodiscard]] bst_node_t GetNumLeaves() const;
  [[nodiscard]] bst_node_t GetNumSplitNodes() const;

  /*!
   * \brief get current depth
   * \param nid node id
   */
  [[nodiscard]] std::int32_t GetDepth(bst_node_t nid) const {
    if (IsMultiTarget()) {
      return this->p_mt_tree_->Depth(nid);
    }
    int depth = 0;
    while (!nodes_[nid].IsRoot()) {
      ++depth;
      nid = nodes_[nid].Parent();
    }
    return depth;
  }
  /**
   * \brief Set the leaf weight for a multi-target tree.
   */
  void SetLeaf(bst_node_t nidx, linalg::VectorView<float const> weight) {
    CHECK(IsMultiTarget());
    return this->p_mt_tree_->SetLeaf(nidx, weight);
  }

  /*!
   * \brief get maximum depth
   * \param nid node id
   */
  [[nodiscard]] int MaxDepth(int nid) const {
    if (nodes_[nid].IsLeaf()) return 0;
    return std::max(MaxDepth(nodes_[nid].LeftChild()) + 1, MaxDepth(nodes_[nid].RightChild()) + 1);
  }

  /*!
   * \brief get maximum depth
   */
  int MaxDepth() { return MaxDepth(0); }

  /*!
   * \brief dense feature vector that can be taken by RegTree
   * and can be construct from sparse feature vector.
   */
  struct FVec {
    /*!
     * \brief initialize the vector with size vector
     * \param size The size of the feature vector.
     */
    void Init(size_t size);
    /*!
     * \brief fill the vector with sparse vector
     * \param inst The sparse instance to fill.
     */
    void Fill(SparsePage::Inst const& inst);

    /*!
     * \brief drop the trace after fill, must be called after fill.
     * \param inst The sparse instance to drop.
     */
    void Drop();
    /*!
     * \brief returns the size of the feature vector
     * \return the size of the feature vector
     */
    [[nodiscard]] size_t Size() const;
    /*!
     * \brief get ith value
     * \param i feature index.
     * \return the i-th feature value
     */
    [[nodiscard]] bst_float GetFvalue(size_t i) const;
    /*!
     * \brief check whether i-th entry is missing
     * \param i feature index.
     * \return whether i-th value is missing.
     */
    [[nodiscard]] bool IsMissing(size_t i) const;
    [[nodiscard]] bool HasMissing() const;
    void HasMissing(bool has_missing) { this->has_missing_ = has_missing; }

    [[nodiscard]] common::Span<float> Data() { return data_; }

   private:
    /**
     * @brief A dense vector for a single sample.
     *
     * It's nan if the value is missing.
     */
    std::vector<float> data_;
    bool has_missing_;
  };

  /*!
   * \brief dump the model in the requested format as a text string
   * \param fmap feature map that may help give interpretations of feature
   * \param with_stats whether dump out statistics as well
   * \param format the format to dump the model in
   * \return the string of dumped model
   */
  [[nodiscard]] std::string DumpModel(const FeatureMap& fmap, bool with_stats,
                                      std::string format) const;
  /*!
   * \brief Get split type for a node.
   * \param nidx Index of node.
   * \return The type of this split.  For leaf node it's always kNumerical.
   */
  [[nodiscard]] FeatureType NodeSplitType(bst_node_t nidx) const { return split_types_.at(nidx); }
  /*!
   * \brief Get split types for all nodes.
   */
  [[nodiscard]] std::vector<FeatureType> const& GetSplitTypes() const {
    return split_types_;
  }
  [[nodiscard]] common::Span<uint32_t const> GetSplitCategories() const {
    return split_categories_;
  }
  /*!
   * \brief Get the bit storage for categories
   */
  [[nodiscard]] common::Span<uint32_t const> NodeCats(bst_node_t nidx) const {
    auto node_ptr = GetCategoriesMatrix().node_ptr;
    auto categories = GetCategoriesMatrix().categories;
    auto segment = node_ptr[nidx];
    auto node_cats = categories.subspan(segment.beg, segment.size);
    return node_cats;
  }
  [[nodiscard]] auto const& GetSplitCategoriesPtr() const { return split_categories_segments_; }

  /**
   * \brief CSR-like matrix for categorical splits.
   *
   * The fields of split_categories_segments_[i] are set such that the range
   * node_ptr[beg:(beg+size)] stores the bitset for the matching categories for the
   * i-th node.
   */
  struct CategoricalSplitMatrix {
    struct Segment {
      std::size_t beg{0};
      std::size_t size{0};
    };
    common::Span<FeatureType const> split_type;
    common::Span<uint32_t const> categories;
    common::Span<Segment const> node_ptr;
  };

  [[nodiscard]] CategoricalSplitMatrix GetCategoriesMatrix() const {
    CategoricalSplitMatrix view;
    view.split_type = common::Span<FeatureType const>(this->GetSplitTypes());
    view.categories = this->GetSplitCategories();
    view.node_ptr = common::Span<CategoricalSplitMatrix::Segment const>(split_categories_segments_);
    return view;
  }

  [[nodiscard]] bst_feature_t SplitIndex(bst_node_t nidx) const {
    if (IsMultiTarget()) {
      return this->p_mt_tree_->SplitIndex(nidx);
    }
    return (*this)[nidx].SplitIndex();
  }
  [[nodiscard]] float SplitCond(bst_node_t nidx) const {
    if (IsMultiTarget()) {
      return this->p_mt_tree_->SplitCond(nidx);
    }
    return (*this)[nidx].SplitCond();
  }
  [[nodiscard]] bool DefaultLeft(bst_node_t nidx) const {
    if (IsMultiTarget()) {
      return this->p_mt_tree_->DefaultLeft(nidx);
    }
    return (*this)[nidx].DefaultLeft();
  }
  [[nodiscard]] bst_node_t DefaultChild(bst_node_t nidx) const {
    return this->DefaultLeft(nidx) ? this->LeftChild(nidx) : this->RightChild(nidx);
  }
  [[nodiscard]] bool IsRoot(bst_node_t nidx) const {
    if (IsMultiTarget()) {
      return nidx == kRoot;
    }
    return (*this)[nidx].IsRoot();
  }
  [[nodiscard]] bool IsLeaf(bst_node_t nidx) const {
    if (IsMultiTarget()) {
      return this->p_mt_tree_->IsLeaf(nidx);
    }
    return (*this)[nidx].IsLeaf();
  }
  [[nodiscard]] bst_node_t Parent(bst_node_t nidx) const {
    if (IsMultiTarget()) {
      return this->p_mt_tree_->Parent(nidx);
    }
    return (*this)[nidx].Parent();
  }
  [[nodiscard]] bst_node_t LeftChild(bst_node_t nidx) const {
    if (IsMultiTarget()) {
      return this->p_mt_tree_->LeftChild(nidx);
    }
    return (*this)[nidx].LeftChild();
  }
  [[nodiscard]] bst_node_t RightChild(bst_node_t nidx) const {
    if (IsMultiTarget()) {
      return this->p_mt_tree_->RightChild(nidx);
    }
    return (*this)[nidx].RightChild();
  }
  [[nodiscard]] bool IsLeftChild(bst_node_t nidx) const {
    if (IsMultiTarget()) {
      CHECK_NE(nidx, kRoot);
      auto p = this->p_mt_tree_->Parent(nidx);
      return nidx == this->p_mt_tree_->LeftChild(p);
    }
    return (*this)[nidx].IsLeftChild();
  }
  [[nodiscard]] bst_node_t Size() const {
    if (IsMultiTarget()) {
      return this->p_mt_tree_->Size();
    }
    return this->nodes_.size();
  }

 private:
  template <bool typed>
  void LoadCategoricalSplit(Json const& in);
  void SaveCategoricalSplit(Json* p_out) const;
  /*! \brief model parameter */
  TreeParam param_;
  // vector of nodes
  std::vector<Node> nodes_;
  // free node space, used during training process
  std::vector<int>  deleted_nodes_;
  // stats of nodes
  std::vector<RTreeNodeStat> stats_;
  std::vector<FeatureType> split_types_;

  // Categories for each internal node.
  std::vector<uint32_t> split_categories_;
  // Ptr to split categories of each node.
  std::vector<CategoricalSplitMatrix::Segment> split_categories_segments_;
  // ptr to multi-target tree with vector leaf.
  CopyUniquePtr<MultiTargetTree> p_mt_tree_;
  // allocate a new node,
  // !!!!!! NOTE: may cause BUG here, nodes.resize
  bst_node_t AllocNode() {
    if (param_.num_deleted != 0) {
      int nid = deleted_nodes_.back();
      deleted_nodes_.pop_back();
      nodes_[nid].Reuse();
      --param_.num_deleted;
      return nid;
    }
    int nd = param_.num_nodes++;
    CHECK_LT(param_.num_nodes, std::numeric_limits<int>::max())
        << "number of nodes in the tree exceed 2^31";
    nodes_.resize(param_.num_nodes);
    stats_.resize(param_.num_nodes);
    split_types_.resize(param_.num_nodes, FeatureType::kNumerical);
    split_categories_segments_.resize(param_.num_nodes);
    return nd;
  }
  // delete a tree node, keep the parent field to allow trace back
  void DeleteNode(int nid) {
    CHECK_GE(nid, 1);
    auto pid = (*this)[nid].Parent();
    if (nid == (*this)[pid].LeftChild()) {
      (*this)[pid].SetLeftChild(kInvalidNodeId);
    } else {
      (*this)[pid].SetRightChild(kInvalidNodeId);
    }

    deleted_nodes_.push_back(nid);
    nodes_[nid].MarkDelete();
    ++param_.num_deleted;
  }
};

inline void RegTree::FVec::Init(size_t size) {
  data_.resize(size);
  std::fill(data_.begin(), data_.end(), std::numeric_limits<float>::quiet_NaN());
  has_missing_ = true;
}

inline void RegTree::FVec::Fill(SparsePage::Inst const& inst) {
  auto p_data = inst.data();
  auto p_out = data_.data();

  for (std::size_t i = 0, n = inst.size(); i < n; ++i) {
    auto const& entry = p_data[i];
    p_out[entry.index] = entry.fvalue;
  }
  has_missing_ = data_.size() != inst.size();
}

inline void RegTree::FVec::Drop() { this->Init(this->Size()); }

inline size_t RegTree::FVec::Size() const {
  return data_.size();
}

inline float RegTree::FVec::GetFvalue(size_t i) const {
  return data_[i];
}

inline bool RegTree::FVec::IsMissing(size_t i) const { return std::isnan(data_[i]); }

inline bool RegTree::FVec::HasMissing() const { return has_missing_; }

// Multi-target tree not yet implemented error
inline StringView MTNotImplemented() {
  return " support for multi-target tree is not yet implemented.";
}
}  // namespace xgboost
#endif  // XGBOOST_TREE_MODEL_H_
