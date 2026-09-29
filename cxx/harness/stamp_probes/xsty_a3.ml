(* two modules rebuilt after a shadowed include *)
include struct type t = int end
type t = float
module Y = struct type v = int end
module Z = struct type v = int end
