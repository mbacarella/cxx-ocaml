module W = struct module F ( X : Set.OrderedType ) = struct module N = Set.Make(
  X ) module P = Set.Make( String ) end end
