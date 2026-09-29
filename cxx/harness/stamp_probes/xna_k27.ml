module X = String module M : sig end = struct module XSet = Set.Make( X ) end
  type u = Set.Make( X ).t
