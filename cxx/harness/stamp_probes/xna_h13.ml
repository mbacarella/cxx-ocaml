module M : sig module XSet : sig type t end end = struct module XSet = Set.Make(
  String ) let x : XSet.t = XSet.empty end
