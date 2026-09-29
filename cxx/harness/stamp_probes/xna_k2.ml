module F ( X : Set.OrderedType ) = struct
  module M : sig module XSet : sig type t end type u = Set.Make( X ).t end =
    struct module XSet = Set.Make( X ) type u = XSet.t end
end
