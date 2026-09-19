module F ( X : Set.OrderedType ) = struct
  module M : sig module XSet : sig type t = Set.Make( X ).t end
               type u = XSet.t end =
    struct module XSet = Set.Make( X ) type u = XSet.t end
end
