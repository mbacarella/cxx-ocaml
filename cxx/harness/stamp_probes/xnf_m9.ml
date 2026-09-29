module type T = functor ( Y : Set.OrderedType ) -> sig type u = Set.Make( Y ).t
  end module type U = T
