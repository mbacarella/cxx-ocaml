(* a bare `t` this unit qualifies itself takes the declaration's name *)
module M : sig type t val v : t end = struct
  type t = A
  let v = A
end
let w = M.v
