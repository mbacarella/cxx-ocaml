module W = struct module F ( X : Set.OrderedType ) = struct let module N =
  Set.Make( X ) in () end end
