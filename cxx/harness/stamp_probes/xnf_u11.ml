module W = struct module F ( X : Set.OrderedType ) = struct module M : sig type
  t = Set.Make( X ).t end = struct type t = Set.Make( X ).t end end end
