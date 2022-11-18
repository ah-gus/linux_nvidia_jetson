import logging
import shutil
from . import tools
from . import common

from pathlib import Path, PurePath

def clean(args, board):
  logging.info(f"Cleaning {board.name} in {board.build_dir}")
  
  shutil.rmtree(board.build_dir)


def prepare(args, board):
  t = tools.tools(args)
  logging.info(f"Preparing {board.name} in {board.build_dir}")
  logging.info("Extracting driver package")
  t.extract(board.files.driver_package, board.build_dir, sudo=True)
  #logging.info("Extracting rootfs")
  #t.extract(board.files.rootfs, board.build_dir / 'Linux_for_Tegra/rootfs', sudo=True)
  logging.warning("Extracting public_sources DISABLED")
  #t.extract(board.files.public_sources, board.build_dir)
  #t.execute(['sudo', './apply_binaries.sh'], cwd=board.build_dir / 'Linux_for_Tegra')
  if board.l4t_patches is not None:
    logging.info("Appling L4T patches")
    base_dir = Path(__file__).parent
    for patch in board.l4t_patches:
      t.execute(['cp', base_dir / 'files/l4t-patches' / patch[0], board.build_dir / 'Linux_for_Tegra' / patch[1]], sudo=True)
