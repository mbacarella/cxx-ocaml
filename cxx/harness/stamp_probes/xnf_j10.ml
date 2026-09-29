module W = struct module F ( X : Set.OrderedType ) = struct type u = Set.Make( X
  ).t type w = u list end end
