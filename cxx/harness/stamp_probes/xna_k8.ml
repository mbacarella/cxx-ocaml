module F ( X : Set.OrderedType ) = struct
  module M : sig end = struct module XSet = Set.Make( X ) end
  type u = Set.Make( X ).t
end
