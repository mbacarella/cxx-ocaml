module W = struct module F ( X : Set.OrderedType ) = functor ( Y :
  Set.OrderedType ) -> struct type u = Set.Make( Y ).t end end
