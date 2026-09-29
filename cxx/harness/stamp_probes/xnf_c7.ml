module W = struct module F ( X : Set.OrderedType ) = struct module N = Set.Make(
  X ) type u = Set.Make( X ).elt end end
