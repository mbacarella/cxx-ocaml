let f x = Atomic.get (if x then assert false else assert false)
