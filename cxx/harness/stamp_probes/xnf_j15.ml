module W = struct module F ( X : Set.OrderedType ) = struct type u = Set.Make( X
  ).t [@@deriving x] end end
