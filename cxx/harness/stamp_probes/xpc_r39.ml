module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class b = object method a : 'a 'b. 'a M.s -> 'b M.s = fun _ -> assert false end
