module W = struct module F ( X : Set.OrderedType ) = struct module type T = sig
  type u end with type u = Set.Make( X ).t end end
