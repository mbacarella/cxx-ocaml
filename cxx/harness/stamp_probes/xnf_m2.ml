module W = struct module type T = functor ( Y : Set.OrderedType ) -> sig type u
  = Set.Make( Y ).t end end
