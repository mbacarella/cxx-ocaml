(* G7: Typeclass.var_option is a module-initialization value: its two
   type nodes are allocated before the first Types reset, not at the
   first class with an optional parameter (every later type id shifts:
   -g debug events, .cmt). *)
class k ?(n = 0) () = object method n = n end
let f (x : k) = x#n + 1
let g l = List.map (fun (x : k) -> x#n) l
