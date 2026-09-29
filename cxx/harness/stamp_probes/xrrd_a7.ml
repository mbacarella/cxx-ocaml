(* a submodule of an included unit keeps its group *)
module M = struct include Seq end
let x : int M.t = M.empty
