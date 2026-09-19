module rec Mod : sig module XSet : Set.S end = struct module XSet = Set.Make(
  String ) end and N : sig end = struct end
