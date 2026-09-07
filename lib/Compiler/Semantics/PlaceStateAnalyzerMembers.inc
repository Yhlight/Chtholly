// Included inside PlaceStateAnalyzer; this fragment aliases its existing
// place-state maps, cleanup plans, and worklists.
  SemIR &sem_ir_;
  std::optional<FunctionId> current_function_;
  std::unordered_set<std::uint32_t> initialized_forwarded_parameters_;
  std::unordered_set<std::uint32_t> projection_inputs_;
  std::unordered_map<std::uint32_t, RootState> states_;
  struct OutcomeFact {
    ResolvedPlace place;
    std::uint32_t variant;
    std::uint8_t outcomes;
    std::uint64_t revision;
    std::uint8_t before;
  };
  std::unordered_map<std::uint32_t, std::uint64_t> place_revisions_;
  std::unordered_map<std::uint32_t, std::vector<OutcomeFact>> outcome_facts_;
  std::unordered_set<std::uint32_t> initialization_attempts_;
  std::vector<LocalId> active_locals_;
  std::vector<CleanupRegistration> cleanup_registrations_;
  std::unordered_map<std::uint32_t, InstId> bound_values_;
  std::unordered_map<std::uint32_t, std::uint8_t> task_value_obligations_;
  std::unordered_map<std::uint32_t, std::uint32_t> task_value_scopes_;
  std::vector<ActiveTaskScope> active_task_scopes_;
  std::uint32_t next_task_scope_ = 1;
  std::vector<SemPlace> places_;
  std::unordered_map<std::string, SemPlaceId> place_ids_;
  std::unordered_map<std::uint32_t, SemPlaceId> move_places_;
  std::unordered_map<std::uint32_t, SemPlaceId> copy_places_;
  std::unordered_map<std::uint32_t, PlaceCleanupPlan> return_cleanups_;
  std::unordered_map<std::uint32_t, PlaceCleanupPlan> edge_cleanups_;
  std::unordered_map<std::uint32_t, PlaceCleanupPlan> full_expression_cleanups_;
  std::unordered_map<std::uint32_t, PlaceCleanupPlan> block_cleanups_;
  std::unordered_map<std::uint32_t, SuspensionCleanupPartition>
      suspension_cleanups_;
  std::unordered_map<std::uint32_t, PlaceCleanupPlan> cancellation_cleanups_;
  std::unordered_map<std::uint32_t, PlaceReinitializationPlan>
      reinitializations_;
  std::vector<PlaceStateObservation> observations_;
  std::vector<PlaceStateDiagnostic> diagnostics_;
  std::vector<LoopFlow> loop_flows_;

  // The place-state fixed-point loops use the same discipline for observations and
  // diagnostics: an iteration is committed only when its state converges.
  struct AnalysisCheckpoint {
    PlaceStateAnalyzer *owner;
    std::size_t observation_mark;
    std::size_t diagnostic_mark;
    bool committed = false;

    explicit AnalysisCheckpoint(PlaceStateAnalyzer &analyzer)
        : owner(&analyzer), observation_mark(analyzer.observations_.size()),
          diagnostic_mark(analyzer.diagnostics_.size()) {}

    AnalysisCheckpoint(const AnalysisCheckpoint &) = delete;
    AnalysisCheckpoint &operator=(const AnalysisCheckpoint &) = delete;

    ~AnalysisCheckpoint() {
      if (!committed) {
        owner->observations_.resize(observation_mark);
        owner->diagnostics_.resize(diagnostic_mark);
      }
    }

    void commit() { committed = true; }
  };
