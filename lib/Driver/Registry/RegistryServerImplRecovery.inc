// Included inside RegistryPublicationStore::Impl; all methods alias its database and config state.
  bool recover(std::string &error) {
    Statement pending(
        database,
        "SELECT mutation_id, package_name, version FROM mutations "
        "WHERE distributed = 0 ORDER BY rowid",
        error);
    if (!pending)
      return false;
    struct Item {
      std::string mutation, package, version;
    };
    std::vector<Item> items;
    while (pending.step() == SQLITE_ROW)
      items.push_back({pending.text(0), pending.text(1), pending.text(2)});
    const auto now = effectiveNow(-1);
    for (const auto &item : items) {
      if (!materialize(item.mutation, item.package, item.version, now, error))
        return false;
    }
    return true;
  }
