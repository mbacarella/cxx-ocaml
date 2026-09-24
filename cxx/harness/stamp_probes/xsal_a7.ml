(* an alias written in a signature *)
module M : sig module L = List end = struct module L = List end
