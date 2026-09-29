module F (X : Set.OrderedType) = struct
  module rec Mod : sig
    module XSet : sig type elt = X.t type t = Set.Make(X).t end
  end = struct module XSet = Set.Make(X) end
end
