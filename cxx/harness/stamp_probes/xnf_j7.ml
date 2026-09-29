module W = struct module F ( X : Set.OrderedType ) = struct module type T =
  functor ( Y : Set.OrderedType ) -> sig type u = Set.Make( Y ).t end end end
