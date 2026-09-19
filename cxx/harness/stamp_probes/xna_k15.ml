module X = String
  module M : sig module XSet : sig type t end end = struct module XSet =
    Set.Make( X ) end
  type u = Set.Make( X ).t
