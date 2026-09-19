module F ( X : Set.OrderedType ) = struct module G ( Y : Set.OrderedType ) =
  struct module type T = functor ( Z : Set.OrderedType ) -> sig type u =
  Set.Make( Z ).t end end end
