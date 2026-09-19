module W = struct module F ( X : Set.OrderedType ) = struct type u = Set.Make( X
  ).t module M : sig type v = Set.Make( X ).t end = struct type v = u end end
  end
