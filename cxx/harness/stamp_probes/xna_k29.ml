module F ( X : Set.OrderedType ) = struct module type T = Set.S with type t =
  Set.Make( X ).t end
