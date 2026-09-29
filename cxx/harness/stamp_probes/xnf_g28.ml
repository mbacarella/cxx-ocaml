module W = struct module F ( X : Set.OrderedType ) = struct module rec M : sig
  type u = Set.Make( X ).t end = struct type u = Set.Make( X ).t end end end
