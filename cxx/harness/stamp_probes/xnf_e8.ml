module W = struct module F ( X : Set.OrderedType ) = struct module N = Set.Make(
  X ) end end module Z : sig end = W
