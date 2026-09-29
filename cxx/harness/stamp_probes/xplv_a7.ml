(* S570 control: marking the binder a scheme must not make a weak variable
   generic -- r is pinned to `int list ref` by the later use *)
type 'a b = B of 'a ref
let (B r) = B (ref [])
let () = r := [1]
