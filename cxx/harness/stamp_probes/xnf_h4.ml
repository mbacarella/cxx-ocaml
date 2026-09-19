module W = struct module F ( X : Set.OrderedType ) = struct module Y = struct
  type t = int let compare = compare end type u = Set.Make( Y ).t end end
