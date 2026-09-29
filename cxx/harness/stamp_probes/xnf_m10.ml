module type T = functor ( Y : Set.OrderedType ) -> sig type u = Set.Make( Y ).t
  end module F : T = functor ( Y : Set.OrderedType ) -> struct type u =
  Set.Make( Y ).t end
