module W = struct module F ( X : Set.OrderedType ) = struct module N = struct
  include Set.Make( X ) end end end
