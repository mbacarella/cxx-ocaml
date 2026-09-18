module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
let f () = let module N : sig class b : object method a : int M.s -> int end
  end = struct class b = object method a : int M.s -> int = fun x -> 0 end end
  in 0
