module F ( X : Set.OrderedType ) = struct type u = Set.Make( X ).t type v =
  Stdlib__Set.Make( X ).t end
