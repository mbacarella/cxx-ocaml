module W = struct module F ( X : Set.OrderedType ) = struct module N =
  Hashtbl.Make( struct type t = X.t let equal = (=) let hash = Hashtbl.hash end
  ) end end
