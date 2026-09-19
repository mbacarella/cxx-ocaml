module W = struct module F ( X : Set.OrderedType ) = struct module N = Set.Make(
  X ) module Q = struct type a end end end
