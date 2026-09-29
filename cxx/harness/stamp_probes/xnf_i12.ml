module W = struct module F ( X : Set.OrderedType ) : sig type u end = struct
  type u = Set.Make( X ).t end end
