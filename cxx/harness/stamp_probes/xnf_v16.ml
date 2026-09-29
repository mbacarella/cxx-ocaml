type v = Set.Make( String ).t module W = struct module F ( X : Set.OrderedType )
  = struct type u = Set.Make( X ).t end end
