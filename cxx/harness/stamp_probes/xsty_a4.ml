(* a nested module is entered twice by the nondep *)
include struct type t = int end
type t = float
module Y = struct module Z = struct type v = int end end
