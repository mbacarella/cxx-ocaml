module type T = sig module F : functor ( Y : Set.OrderedType ) -> sig type u =
  Set.Make( Y ).t end end
