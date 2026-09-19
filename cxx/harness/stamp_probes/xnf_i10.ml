module W = struct module F : functor ( X : Set.OrderedType ) -> sig type u =
  Set.Make( X ).t end = functor ( X : Set.OrderedType ) -> struct type u =
  Set.Make( X ).t end end
